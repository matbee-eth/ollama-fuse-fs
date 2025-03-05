// Include our compatibility header first
#include "win_compat.h"
#include "win_thread.h"

// Standard C++ includes
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include "json.hpp"

// Windows-specific includes for resource extraction
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// Global variables
std::string g_mount_point;
std::string g_ollama_dir = DEFAULT_OLLAMA_DIR;
bool g_verbose = false;
bool g_read_only = false;
bool g_disable_monitoring = false;
bool g_show_hidden = false;
bool g_show_help = false;
bool g_show_version = false;
bool g_running = true;
win_thread::thread_type g_monitor_thread;
win_thread::mutex_type g_models_mutex;

// Platform-specific includes

// Data structures to represent the Ollama filesystem
struct ModelInfo {
    std::string name;           // Model name (e.g., "llama3.2")
    std::string version;        // Model version (e.g., "latest")
    std::string path;           // Path to the model directory
    std::string modelfile_path; // Path to the Modelfile
    json manifest;              // Parsed manifest.json data
    std::map<std::string, std::string> file_mappings; // Maps virtual paths to real paths
};

// Global variables
static std::vector<std::string> g_ollama_dirs;
static std::map<std::string, ModelInfo> g_models;
static std::map<std::string, std::string> g_file_mappings; // Maps virtual paths to real paths
static bool g_debug_mode = false; // Debug mode flag
static bool g_generate_llama_swap = false; // Flag to generate llama-swap config
static std::string g_llama_swap_base_url = "http://0.0.0.0:11434"; // Default base URL for llama-swap

// Forward declarations
static bool discover_ollama_models();

// Filesystem monitoring variables
static std::atomic<bool> g_monitoring_enabled(true);

#ifdef _WIN32
    // Windows-specific filesystem monitoring variables
    static HANDLE g_change_handle = INVALID_HANDLE_VALUE;
#else
    // Linux-specific filesystem monitoring variables
    static int g_inotify_fd = -1;
    static int g_watch_descriptor = -1;
#endif

// Helper functions
static std::string get_filename(const std::string& path) {
    size_t pos = path.find_last_of(PATH_SEPARATOR);
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

static std::string get_dirname(const std::string& path) {
    size_t pos = path.find_last_of(PATH_SEPARATOR);
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

// Read a file into a string
static std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << path << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Platform-specific directory listing function
#ifdef _WIN32
static bool list_directory(const std::string& dir_path, std::vector<std::string>& files, std::vector<std::string>& dirs) {
    WIN32_FIND_DATAW find_data;
    std::wstring search_path = std::wstring(dir_path.begin(), dir_path.end()) + L"\\*";
    
    HANDLE find_handle = FindFirstFileW(search_path.c_str(), &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    do {
        std::wstring name(find_data.cFileName);
        if (name == L"." || name == L"..") continue;
        
        std::string utf8_name(name.begin(), name.end()); // Simple conversion, might need improvement
        
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs.push_back(utf8_name);
        } else {
            files.push_back(utf8_name);
        }
    } while (FindNextFileW(find_handle, &find_data));
    
    FindClose(find_handle);
    return true;
}
#else
static bool list_directory(const std::string& dir_path, std::vector<std::string>& files, std::vector<std::string>& dirs) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        return false;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        std::string full_path = dir_path + "/" + entry->d_name;
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                dirs.push_back(entry->d_name);
            } else {
                files.push_back(entry->d_name);
            }
        }
    }
    
    closedir(dir);
    return true;
}
#endif

// Platform-specific filesystem monitoring implementation
#ifdef _WIN32
// Function to initialize filesystem monitoring on Windows
static bool init_filesystem_monitoring() {
    // For Windows, we'll use FindFirstChangeNotification/FindNextChangeNotification
    bool any_watch_added = false;
    
    for (const auto& ollama_dir : g_ollama_dirs) {
        std::string manifests_dir = ollama_dir + "\\models\\manifests";
        
        // Convert to wide string for Windows API
        std::wstring wide_path(manifests_dir.begin(), manifests_dir.end());
        
        g_change_handle = FindFirstChangeNotificationW(
            wide_path.c_str(),
            TRUE,  // Watch subdirectories
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE
        );
        
        if (g_change_handle != INVALID_HANDLE_VALUE) {
            any_watch_added = true;
            std::cout << "Added watch for directory: " << manifests_dir << std::endl;
            break; // For simplicity, we'll just watch the first valid directory
        } else {
            std::cerr << "Failed to add watch for directory: " << manifests_dir << std::endl;
        }
    }
    
    return any_watch_added;
}

// Function to stop filesystem monitoring on Windows
static void stop_filesystem_monitoring() {
    if (g_monitoring_enabled.exchange(false)) {
        if (g_monitor_thread.joinable()) {
            g_monitor_thread.join();
        }
    }
    
    if (g_change_handle != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(g_change_handle);
        g_change_handle = INVALID_HANDLE_VALUE;
    }
}

// Function to monitor filesystem changes on Windows
static void monitor_filesystem_changes() {
    g_monitoring_enabled = true;
    
    while (g_monitoring_enabled) {
        // Wait for a change notification with a timeout
        DWORD wait_result = WaitForSingleObject(g_change_handle, 1000); // 1 second timeout
        
        if (wait_result == WAIT_OBJECT_0) {
            // A change was detected
            if (g_verbose) {
                std::cout << "Filesystem change detected in " << g_ollama_dir << std::endl;
            }
            
            // Store the current models for comparison
            std::set<std::string> old_models;
            {
                win_thread::lock_guard_type lock(g_models_mutex);
                for (const auto& model : g_models) {
                    old_models.insert(model.first);
                }
            }
            
            // Rediscover models
            discover_ollama_models();
            
            // Check for new models
            std::vector<std::string> new_models;
            {
                win_thread::lock_guard_type lock(g_models_mutex);
                for (const auto& model : g_models) {
                    if (old_models.find(model.first) == old_models.end()) {
                        new_models.push_back(model.first);
                    }
                }
            }
            
            std::cout << "Successfully refreshed Ollama models" << std::endl;
            
            // Print new models
            if (!new_models.empty()) {
                std::cout << "New models found:" << std::endl;
                for (const auto& model : new_models) {
                    std::cout << "  " << model << std::endl;
                }
            }
            
            // Reset the change notification
            if (!FindNextChangeNotification(g_change_handle)) {
                std::cerr << "FindNextChangeNotification failed" << std::endl;
                break;
            }
        } else if (wait_result == WAIT_TIMEOUT) {
            // Timeout, continue the loop
            continue;
        } else {
            // Error
            std::cerr << "WaitForSingleObject failed" << std::endl;
            break;
        }
    }
}
#else
// Function to add watches recursively to a directory and its subdirectories on Linux
static void add_watches_recursive(const std::string& dir_path) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        std::cerr << "Failed to open directory for watching: " << dir_path << ": " << strerror(errno) << std::endl;
        return;
    }
    
    // Add watch for this directory
    int wd = inotify_add_watch(g_inotify_fd, dir_path.c_str(), 
                             IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
    if (wd == -1) {
        std::cerr << "Failed to add watch for directory: " << dir_path << ": " << strerror(errno) << std::endl;
    } else {
        std::cout << "Added watch for directory: " << dir_path << std::endl;
    }
    
    // Recursively add watches to subdirectories
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR && 
            strcmp(entry->d_name, ".") != 0 && 
            strcmp(entry->d_name, "..") != 0) {
            std::string subdir_path = dir_path + "/" + entry->d_name;
            add_watches_recursive(subdir_path);
        }
    }
    
    closedir(dir);
}

// Function to initialize inotify monitoring on Linux
static bool init_filesystem_monitoring() {
    // Initialize inotify
    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd == -1) {
        std::cerr << "Failed to initialize inotify: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Add watches for each Ollama directory
    bool any_watch_added = false;
    for (const auto& ollama_dir : g_ollama_dirs) {
        // Add watches recursively for the manifests directory and its subdirectories
        std::string manifests_dir = ollama_dir + "/models/manifests";
        add_watches_recursive(manifests_dir);
        
        // Store the main watch descriptor for cleanup (using the last one for simplicity)
        g_watch_descriptor = inotify_add_watch(g_inotify_fd, manifests_dir.c_str(), 
                                             IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
        if (g_watch_descriptor != -1) {
            any_watch_added = true;
        } else {
            std::cerr << "Failed to add main watch for directory: " << manifests_dir << ": " << strerror(errno) << std::endl;
        }
    }
    
    if (!any_watch_added) {
        std::cerr << "Failed to add watches for any Ollama directory" << std::endl;
        close(g_inotify_fd);
        g_inotify_fd = -1;
        return false;
    }
    
    return true;
}

// Function to stop filesystem monitoring on Linux
static void stop_filesystem_monitoring() {
    if (g_monitoring_enabled.exchange(false)) {
        if (g_monitor_thread.joinable()) {
            g_monitor_thread.join();
        }
    }
    
    if (g_watch_descriptor != -1) {
        inotify_rm_watch(g_inotify_fd, g_watch_descriptor);
        g_watch_descriptor = -1;
    }
    
    if (g_inotify_fd != -1) {
        close(g_inotify_fd);
        g_inotify_fd = -1;
    }
}

// Function to monitor filesystem changes on Linux
static void monitor_filesystem_changes() {
    const size_t BUF_LEN = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
    char buffer[BUF_LEN];
    struct pollfd pfd = { g_inotify_fd, POLLIN, 0 };
    
    g_monitoring_enabled = true;
    
    while (g_monitoring_enabled) {
        // Poll with a timeout of 1 second
        int poll_result = poll(&pfd, 1, 1000);
        
        if (poll_result == -1) {
            if (errno == EINTR) continue; // Interrupted, try again
            std::cerr << "Poll error: " << strerror(errno) << std::endl;
            break;
        }
        
        if (poll_result > 0 && (pfd.revents & POLLIN)) {
            // Read events
            ssize_t len = read(g_inotify_fd, buffer, BUF_LEN);
            if (len == -1 && errno != EAGAIN) {
                std::cerr << "Read error: " << strerror(errno) << std::endl;
                break;
            }
            
            if (len <= 0) continue;
            
            // Process events
            bool changes_detected = false;
            for (char* ptr = buffer; ptr < buffer + len; ) {
                struct inotify_event* event = (struct inotify_event*)ptr;
                
                // Check if this is a relevant event
                if (event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY)) {
                    changes_detected = true;
                    break;
                }
                
                ptr += sizeof(struct inotify_event) + event->len;
            }
            
            if (changes_detected) {
                if (g_debug_mode) {
                    std::cout << "Changes detected in Ollama models directory, refreshing..." << std::endl;
                }
                
                // Save the current list of models for comparison
                std::set<std::string> old_models;
                {
                    win_thread::lock_guard_type lock(g_models_mutex);
                    for (const auto& model : g_models) {
                        old_models.insert(model.first);
                    }
                }
                
                // Call discover_ollama_models to refresh the model list
                if (!discover_ollama_models()) {
                    std::cerr << "Failed to refresh Ollama models" << std::endl;
                } else {
                    // Compare old and new models to find new ones
                    std::vector<std::string> new_models;
                    {
                        win_thread::lock_guard_type lock(g_models_mutex);
                        for (const auto& model : g_models) {
                            if (old_models.find(model.first) == old_models.end()) {
                                new_models.push_back(model.first);
                            }
                        }
                    }
                    
                    std::cout << "Successfully refreshed Ollama models" << std::endl;
                    
                    // Print new models
                    if (!new_models.empty()) {
                        std::cout << "New models found:" << std::endl;
                        for (const auto& model : new_models) {
                            std::cout << "  " << model << std::endl;
                        }
                    }
                }
            }
        }
    }
}
#endif

// Function to start filesystem monitoring
void start_filesystem_monitoring() {
    if (g_disable_monitoring) {
        std::cout << "Filesystem monitoring disabled by user" << std::endl;
        return;
    }
    
    if (init_filesystem_monitoring()) {
        std::cout << "Filesystem monitoring started" << std::endl;
        g_monitor_thread = win_thread::thread_type(monitor_filesystem_changes);
    } else {
        std::cerr << "Failed to start filesystem monitoring" << std::endl;
    }
}

// Function to discover Ollama models
static bool discover_ollama_models() {
    // Lock the mutex to ensure thread safety
    win_thread::lock_guard_type lock(g_models_mutex);
    
    // Clear existing models and file mappings
    g_models.clear();
    g_file_mappings.clear();
    
    bool any_models_found = false;
    
    // Process each Ollama directory
    for (const auto& ollama_dir : g_ollama_dirs) {
        std::string manifests_dir = ollama_dir + "/models/manifests";
        DIR* dir = opendir(manifests_dir.c_str());
        if (!dir) {
            std::cerr << "Failed to open Ollama manifests directory: " << manifests_dir << std::endl;
            continue; // Try the next directory
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string registry_dir = manifests_dir + "/" + entry->d_name;
            DIR* reg_dir = opendir(registry_dir.c_str());
            if (!reg_dir) continue;

            struct dirent* reg_entry;
            while ((reg_entry = readdir(reg_dir)) != nullptr) {
                if (reg_entry->d_type != DT_DIR || strcmp(reg_entry->d_name, ".") == 0 || strcmp(reg_entry->d_name, "..") == 0) {
                    continue;
                }

                std::string library_dir = registry_dir + "/" + reg_entry->d_name;
                DIR* lib_dir = opendir(library_dir.c_str());
                if (!lib_dir) continue;

                struct dirent* lib_entry;
                while ((lib_entry = readdir(lib_dir)) != nullptr) {
                    if (lib_entry->d_type != DT_DIR || strcmp(lib_entry->d_name, ".") == 0 || strcmp(lib_entry->d_name, "..") == 0) {
                        continue;
                    }

                    std::string model_dir = library_dir + "/" + lib_entry->d_name;
                    DIR* model_dir_handle = opendir(model_dir.c_str());
                    if (!model_dir_handle) continue;

                    struct dirent* version_entry;
                    while ((version_entry = readdir(model_dir_handle)) != nullptr) {
                        if (version_entry->d_type != DT_REG || strcmp(version_entry->d_name, ".") == 0 || strcmp(version_entry->d_name, "..") == 0) {
                            continue;
                        }

                        std::string manifest_path = model_dir + "/" + version_entry->d_name;
                        std::string model_name = lib_entry->d_name;
                        std::string version = version_entry->d_name;

                        // Read and parse the manifest file
                        std::string manifest_content = read_file(manifest_path);
                        if (manifest_content.empty()) continue;

                        try {
                            json manifest_json = json::parse(manifest_content);
                            
                            ModelInfo model;
                            model.name = model_name;
                            model.version = version;
                            model.path = model_dir;
                            model.manifest = manifest_json;
                            
                            // Find the GGUF file in the blobs directory
                            if (manifest_json.contains("layers") && manifest_json["layers"].is_array()) {
                                for (const auto& layer : manifest_json["layers"]) {
                                    if (layer.contains("mediaType") && 
                                        layer["mediaType"].get<std::string>() == "application/vnd.ollama.image.model") {
                                        if (layer.contains("digest")) {
                                            std::string digest = layer["digest"].get<std::string>();
                                            // Format the digest for the filename
                                            std::string digest_filename = digest;
                                            if (digest_filename.find("sha256:") == 0) {
                                                digest_filename = "sha256-" + digest_filename.substr(7);
                                            }
                                            // Try to find the GGUF file in any of the Ollama directories
                                            for (const auto& ollama_dir : g_ollama_dirs) {
                                                std::string potential_path = ollama_dir + "/models/blobs/" + digest_filename;
                                                struct stat st;
                                                if (stat(potential_path.c_str(), &st) == 0) {
                                                    model.file_mappings["/" + model_name + "/" + version + ".gguf"] = potential_path;
                                                    break;
                                                }
                                            }
                                            
                                            // Check if the file exists
                                            struct stat st;
                                            if (stat(model.file_mappings["/" + model_name + "/" + version + ".gguf"].c_str(), &st) != 0) {
                                                std::cerr << "Warning: GGUF file not found at " << model.file_mappings["/" + model_name + "/" + version + ".gguf"] << std::endl;
                                            } else {
                                                std::cout << "Found GGUF file at " << model.file_mappings["/" + model_name + "/" + version + ".gguf"] << " (" << (st.st_size / (1024*1024)) << " MB)" << std::endl;
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                            
                            // Add the model to our list
                            g_models[model_name + ":" + version] = model;
                            
                            // Create file mappings
                            std::string virtual_gguf_path = "/" + model_name + "/" + version + ".gguf";
                            std::string virtual_modelfile_path = "/" + model_name + "/" + version + ".modelfile";
                            
                            if (!model.file_mappings.empty()) {
                                for (const auto& mapping : model.file_mappings) {
                                    g_file_mappings[mapping.first] = mapping.second;
                                }
                            }
                            
                            // Create a ModelFile from the manifest and other files
                            std::string modelfile_content;
                            
                            // Get the config file
                            if (manifest_json.contains("config") && manifest_json["config"].contains("digest")) {
                                std::string config_digest = manifest_json["config"]["digest"].get<std::string>();
                                std::string config_filename = config_digest;
                                if (config_filename.find("sha256:") == 0) {
                                    config_filename = "sha256-" + config_filename.substr(7);
                                }
                                // Try to find the config file in any of the Ollama directories
                                std::string config_path;
                                for (const auto& ollama_dir : g_ollama_dirs) {
                                    std::string potential_path = ollama_dir + "/models/blobs/" + config_filename;
                                    struct stat st;
                                    if (stat(potential_path.c_str(), &st) == 0) {
                                        config_path = potential_path;
                                        break;
                                    }
                                }
                                std::string config_content = read_file(config_path);
                                
                                if (!config_content.empty()) {
                                    try {
                                        json config_json = json::parse(config_content);
                                        if (config_json.contains("model_family")) {
                                            modelfile_content += "FROM " + config_json["model_family"].get<std::string>() + "\n\n";
                                        }
                                    } catch (const std::exception& e) {
                                        std::cerr << "Error parsing config file: " << e.what() << std::endl;
                                    }
                                }
                            }
                            
                            // Add model name and version
                            modelfile_content += "# Model: " + model_name + "\n";
                            modelfile_content += "# Version: " + version + "\n\n";
                            
                            // Process layers to find template, parameters, etc.
                            if (manifest_json.contains("layers") && manifest_json["layers"].is_array()) {
                                for (const auto& layer : manifest_json["layers"]) {
                                    if (layer.contains("mediaType") && layer.contains("digest")) {
                                        std::string media_type = layer["mediaType"].get<std::string>();
                                        std::string digest = layer["digest"].get<std::string>();
                                        std::string digest_filename = digest;
                                        if (digest_filename.find("sha256:") == 0) {
                                            digest_filename = "sha256-" + digest_filename.substr(7);
                                        }
                                        // Try to find the blob file in any of the Ollama directories
                                        std::string blob_path;
                                        for (const auto& ollama_dir : g_ollama_dirs) {
                                            std::string potential_path = ollama_dir + "/models/blobs/" + digest_filename;
                                            struct stat st;
                                            if (stat(potential_path.c_str(), &st) == 0) {
                                                blob_path = potential_path;
                                                break;
                                            }
                                        }
                                        
                                        if (media_type == "application/vnd.ollama.image.template") {
                                            std::string template_content = read_file(blob_path);
                                            if (!template_content.empty()) {
                                                modelfile_content += "TEMPLATE \"\"\"" + template_content + "\"\"\"\n\n";
                                            }
                                        } else if (media_type == "application/vnd.ollama.image.params") {
                                            std::string params_content = read_file(blob_path);
                                            if (!params_content.empty()) {
                                                try {
                                                    json params_json = json::parse(params_content);
                                                    // Iterate through all parameters in the JSON
                                                    for (auto& [param_name, param_value] : params_json.items()) {
                                                        if (param_value.is_array()) {
                                                            // For array parameters, add each value as a separate parameter
                                                            for (size_t i = 0; i < param_value.size(); ++i) {
                                                                if (param_value[i].is_string()) {
                                                                    modelfile_content += "PARAMETER " + param_name + " \"" + param_value[i].get<std::string>() + "\"\n";
                                                                } else if (param_value[i].is_number()) {
                                                                    modelfile_content += "PARAMETER " + param_name + " " + param_value[i].dump() + "\n";
                                                                } else if (param_value[i].is_boolean()) {
                                                                    modelfile_content += "PARAMETER " + param_name + " " + (param_value[i].get<bool>() ? "true" : "false") + "\n";
                                                                }
                                                            }
                                                        } else if (param_value.is_string()) {
                                                            // For string parameters
                                                            modelfile_content += "PARAMETER " + param_name + " \"" + param_value.get<std::string>() + "\"\n";
                                                        } else if (param_value.is_number()) {
                                                            // For numeric parameters
                                                            modelfile_content += "PARAMETER " + param_name + " " + param_value.dump() + "\n";
                                                        } else if (param_value.is_boolean()) {
                                                            // For boolean parameters
                                                            modelfile_content += "PARAMETER " + param_name + " " + (param_value.get<bool>() ? "true" : "false") + "\n";
                                                        }
                                                    }
                                                } catch (const std::exception& e) {
                                                    std::cerr << "Error parsing parameters file: " << e.what() << std::endl;
                                                }
                                            }
                                        } else if (media_type == "application/vnd.ollama.image.license") {
                                            std::string license_content = read_file(blob_path);
                                            if (!license_content.empty()) {
                                                modelfile_content += "LICENSE \"\"\"\n" + license_content + "\"\"\"\n\n";
                                            }
                                        }
                                    }
                                }
                            }
                            
                            // Create a temporary file for the ModelFile
                            std::string temp_modelfile = "/tmp/ollama_" + model_name + "_" + version + ".modelfile";
                            std::ofstream modelfile(temp_modelfile);
                            if (modelfile.is_open()) {
                                modelfile << modelfile_content;
                                modelfile.close();
                                model.modelfile_path = temp_modelfile;
                                g_file_mappings[virtual_modelfile_path] = temp_modelfile;
                                
                                // Check if the file exists
                                struct stat st;
                                if (stat(model.modelfile_path.c_str(), &st) != 0) {
                                    std::cerr << "Warning: Modelfile not found at " << model.modelfile_path << std::endl;
                                } else {
                                    std::cout << "Created Modelfile at " << model.modelfile_path << " (" << st.st_size << " bytes)" << std::endl;
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Error parsing manifest for model " << model_name << ": " << e.what() << std::endl;
                            continue;
                        }
                    }
                    closedir(model_dir_handle);
                }
                closedir(lib_dir);
            }
            closedir(reg_dir);
        }
        closedir(dir);
        any_models_found = true;
    }

    std::cout << "Discovered " << g_models.size() << " Ollama models" << std::endl;
    
    if (g_debug_mode) {
        for (const auto& model : g_models) {
            std::cout << "Model: " << model.first << std::endl;
        }
        
        // Print file mappings for debugging
        std::cout << "\nFile mappings:" << std::endl;
        for (const auto& mapping : g_file_mappings) {
            std::cout << "Virtual path: " << mapping.first << " -> Real path: " << mapping.second << std::endl;
        }
    }

    return !g_models.empty();
}

#ifdef _WIN32
// Temporarily undefine stat macro to avoid issues with function signatures
#undef stat
#endif

// FUSE operations
static int ollama_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    // Lock the mutex to ensure thread safety
    win_thread::lock_guard_type lock(g_models_mutex);

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    // Check if this is a model directory
    for (const auto& model : g_models) {
        std::string model_dir = "/" + model.first;
        if (strcmp(path, model_dir.c_str()) == 0) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            return 0;
        }
    }

    // Check if this is a file in our mappings
    if (g_file_mappings.find(path) != g_file_mappings.end()) {
        const std::string& real_path = g_file_mappings[path];
#ifdef _WIN32
        struct _stat64 real_stat;
        if (_stat64(real_path.c_str(), &real_stat) == 0) {
#else
        struct stat real_stat;
        if (stat(real_path.c_str(), &real_stat) == 0) {
#endif
            stbuf->st_mode = S_IFREG | 0444;
            stbuf->st_nlink = 1;
            stbuf->st_size = real_stat.st_size;
            stbuf->st_atime = real_stat.st_atime;
            stbuf->st_mtime = real_stat.st_mtime;
            stbuf->st_ctime = real_stat.st_ctime;
            return 0;
        }
    }

    return -ENOENT;
}

#ifdef _WIN32
// Redefine stat macro
#define stat _stat64
#endif

static int ollama_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    // Lock the mutex to ensure thread safety
    win_thread::lock_guard_type lock(g_models_mutex);

    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));

    if (strcmp(path, "/") == 0) {
        // Root directory - list all model names
        std::set<std::string> unique_models;
        for (const auto& model : g_models) {
            unique_models.insert(model.first);
        }
        
        for (const auto& model_name : unique_models) {
            filler(buf, model_name.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        }
        
        return 0;
    }

    // Check if this is a model directory
    std::string path_str(path);
    if (path_str.length() > 1 && path_str[0] == '/') {
        std::string model_name = path_str.substr(1); // Remove leading '/'
        
        // List all versions for this model
        for (const auto& model : g_models) {
            if (model.first.find(model_name) != std::string::npos) {
                std::string gguf_file = model.first.substr(model_name.length() + 1);
                std::string modelfile = model.first.substr(model_name.length() + 1) + ".modelfile";
                
                filler(buf, gguf_file.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
                filler(buf, modelfile.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
            }
        }
        
        return 0;
    }

    return -ENOENT;
}

static int ollama_open(const char* path, struct fuse_file_info* fi) {
    // Lock the mutex to ensure thread safety
    win_thread::lock_guard_type lock(g_models_mutex);
    
    if (g_file_mappings.find(path) == g_file_mappings.end()) {
        return -ENOENT;
    }
    
    // Check if we have read permission
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
    }
    
    return 0;
}

static int ollama_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)fi;
    
    // Lock the mutex to ensure thread safety
    win_thread::lock_guard_type lock(g_models_mutex);
    
    if (g_file_mappings.find(path) == g_file_mappings.end()) {
        return -ENOENT;
    }
    
    const std::string& real_path = g_file_mappings[path];
    int fd = open(real_path.c_str(), O_RDONLY);
    if (fd == -1) {
        return -errno;
    }
    
    ssize_t res = pread(fd, buf, size, offset);
    close(fd);
    
    if (res == -1) {
        return -errno;
    }
    
    return res;
}

#ifdef _WIN32
// Temporarily undefine mkdir, access, and stat macros to avoid issues with fuse_operations struct
#undef mkdir
#undef access
#undef stat
#endif

static const struct fuse_operations ollama_oper = {
    .getattr = ollama_getattr,
    .readlink = nullptr,
    .mknod = nullptr,
    .mkdir = nullptr,
    .unlink = nullptr,
    .rmdir = nullptr,
    .symlink = nullptr,
    .rename = nullptr,
    .link = nullptr,
    .chmod = nullptr,
    .chown = nullptr,
    .truncate = nullptr,
    .open = ollama_open,
    .read = ollama_read,
    .write = nullptr,
    .statfs = nullptr,
    .flush = nullptr,
    .release = nullptr,
    .fsync = nullptr,
    .setxattr = nullptr,
    .getxattr = nullptr,
    .listxattr = nullptr,
    .removexattr = nullptr,
    .opendir = nullptr,
    .readdir = ollama_readdir,
    .releasedir = nullptr,
    .fsyncdir = nullptr,
    .init = nullptr,
    .destroy = nullptr,
    .access = nullptr,
    .create = nullptr,
    .lock = nullptr,
    .utimens = nullptr,
    .bmap = nullptr,
    .ioctl = nullptr,
    .poll = nullptr,
    .write_buf = nullptr,
    .read_buf = nullptr,
    .flock = nullptr,
    .fallocate = nullptr,
#ifndef _WIN32
    .copy_file_range = nullptr,
#endif
};

#ifdef _WIN32
// Redefine mkdir, access, and stat macros after fuse_operations struct
#define mkdir mkdir_compat
#define access _access
#define stat _stat64
#endif

// Function to generate llama-swap config.yaml
static void generate_llama_swap_config(const std::string& output_path) {
    std::ofstream config_file(output_path);
    if (!config_file.is_open()) {
        std::cerr << "Failed to open " << output_path << " for writing" << std::endl;
        return;
    }
    
    // Write header
    config_file << "# llama-swap configuration generated by ollama-fuse\n";
    config_file << "# Generated on " << std::time(nullptr) << "\n\n";
    
    // Write default settings
    config_file << "# Seconds to wait for llama.cpp to load and be ready to serve requests\n";
    config_file << "healthCheckTimeout: 60\n\n";
    
    config_file << "# Write HTTP logs (useful for troubleshooting)\n";
    config_file << "logRequests: true\n\n";
    
    // Write models section
    config_file << "# Define valid model values and the upstream server start\n";
    config_file << "models:\n";
    
    // Add each model
    win_thread::lock_guard_type lock(g_models_mutex);
    for (const auto& model : g_models) {
        std::string model_key = model.first;
        if (!model.second.version.empty() && model.second.version != "latest") {
            model_key += "-" + model.second.version;
        }
        
        config_file << "  \"" << model_key << "\":\n";
        
        // Find the GGUF file path
        std::string gguf_path;
        for (const auto& mapping : g_file_mappings) {
            if (mapping.first.find("/" + model.first + "/" + model.second.version + ".gguf") != std::string::npos) {
                gguf_path = mapping.second;
                break;
            }
        }
        
        // Add proxy
        config_file << "    proxy: \"" << g_llama_swap_base_url << "\"\n";
        
        // Add command
        config_file << "    cmd: >\n";
        config_file << "      /app/llama-server\n";
        
        if (!gguf_path.empty()) {
            config_file << "      -m " << gguf_path << "\n";
        } else {
            // If we can't find the exact GGUF file, use the model name/version
            config_file << "      -m " << model.first << "/" << model.second.version << "\n";
        }
        
        // Add port (using a unique port for each model)
        config_file << "      --port 9999\n";
        config_file << "\n";
    }
    
    config_file.close();
    std::cout << "Generated llama-swap config at: " << output_path << std::endl;
}

#ifdef _WIN32
// Declare fuse_main_real before using it in the macro
extern "C" int fuse_main_real(int argc, char *argv[],
    const struct fuse_operations *ops, size_t opsize, void *data);

// Define our own fuse_main macro for Windows
#undef fuse_main
#define fuse_main(argc, argv, ops, data)\
    fuse_main_real(argc, argv, ops, sizeof *(ops), data)
#endif

#ifdef EMBED_WINFSP_DLLS
// Function to extract embedded DLLs
static bool extract_embedded_dlls() {
    #ifdef _WIN32
    // Get the temporary directory path
    char temp_path[MAX_PATH];
    if (GetTempPathA(MAX_PATH, temp_path) == 0) {
        std::cerr << "Failed to get temporary directory path" << std::endl;
        return false;
    }
    
    // Create a subdirectory for our DLLs
    std::string dll_dir = std::string(temp_path) + "ollama-fuse-dlls";
    CreateDirectoryA(dll_dir.c_str(), NULL);
    
    // Extract the platform-specific DLL
    HRSRC platform_dll_res = FindResourceA(NULL, "WINFSP_PLATFORM_DLL", "RCDATA");
    if (platform_dll_res) {
        HGLOBAL platform_dll_handle = LoadResource(NULL, platform_dll_res);
        if (platform_dll_handle) {
            DWORD platform_dll_size = SizeofResource(NULL, platform_dll_res);
            void* platform_dll_data = LockResource(platform_dll_handle);
            
            if (platform_dll_data) {
                std::string platform_dll_path = dll_dir + "\\winfsp-platform.dll";
                std::ofstream platform_dll_file(platform_dll_path, std::ios::binary);
                platform_dll_file.write(static_cast<const char*>(platform_dll_data), platform_dll_size);
                platform_dll_file.close();
                
                // Load the DLL
                if (LoadLibraryA(platform_dll_path.c_str()) == NULL) {
                    std::cerr << "Failed to load platform DLL: " << GetLastError() << std::endl;
                    return false;
                }
            }
        }
    } else {
        std::cerr << "Failed to find platform DLL resource: " << GetLastError() << std::endl;
        return false;
    }
    
    // Extract the MSIL DLL
    HRSRC msil_dll_res = FindResourceA(NULL, "WINFSP_MSIL_DLL", "RCDATA");
    if (msil_dll_res) {
        HGLOBAL msil_dll_handle = LoadResource(NULL, msil_dll_res);
        if (msil_dll_handle) {
            DWORD msil_dll_size = SizeofResource(NULL, msil_dll_res);
            void* msil_dll_data = LockResource(msil_dll_handle);
            
            if (msil_dll_data) {
                std::string msil_dll_path = dll_dir + "\\winfsp-msil.dll";
                std::ofstream msil_dll_file(msil_dll_path, std::ios::binary);
                msil_dll_file.write(static_cast<const char*>(msil_dll_data), msil_dll_size);
                msil_dll_file.close();
                
                // Load the DLL
                if (LoadLibraryA(msil_dll_path.c_str()) == NULL) {
                    std::cerr << "Failed to load MSIL DLL: " << GetLastError() << std::endl;
                    return false;
                }
            }
        }
    } else {
        std::cerr << "Failed to find MSIL DLL resource: " << GetLastError() << std::endl;
        return false;
    }
    
    return true;
    #else
    return false;
    #endif
}
#endif

int main(int argc, char* argv[]) {
    // Check if we have enough arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <mount_point> [ollama_path] [options]" << std::endl;
        std::cerr << "\nOptions:" << std::endl;
        std::cerr << "  --ollama-dir=<path>  Path to Ollama directory (can be specified multiple times)" << std::endl;
        std::cerr << "  -f                   Run in foreground (don't fork)" << std::endl;
        std::cerr << "  -d                   Enable debug output" << std::endl;
        std::cerr << "  -s                   Run single-threaded" << std::endl;
        std::cerr << "  --llama-swap, -ls    Generate llama-swap config" << std::endl;
        std::cerr << "  --base-url <url>     Base URL for llama-swap (default: http://localhost:11434)" << std::endl;
        std::cerr << "  -o opt,[opt]   FUSE mount options" << std::endl;
        std::cerr << "\nEnvironment Variables:" << std::endl;
        std::cerr << "  OLLAMA_HOME    Path to Ollama directory (used if no ollama_path specified)" << std::endl;
        return 1;
    }
    
#ifdef EMBED_WINFSP_DLLS
    // Extract and load embedded DLLs
    if (!extract_embedded_dlls()) {
        std::cerr << "Failed to extract embedded WinFSP DLLs" << std::endl;
        return 1;
    }
    
    if (g_debug_mode) {
        std::cout << "Successfully extracted and loaded WinFSP DLLs" << std::endl;
    }
#endif
    
    // Prepare FUSE arguments
    std::vector<char*> fuse_args;
    fuse_args.push_back(argv[0]);  // Program name
    fuse_args.push_back(argv[1]);  // Mount point
    
    // Process our own arguments
    std::vector<std::string> ollama_dirs;
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            // Pass through FUSE options
            fuse_args.push_back(argv[i]);
            if (i + 1 < argc) {
                fuse_args.push_back(argv[i+1]);
                i++;
            }
        } else if (strcmp(argv[i], "-f") == 0) {
            // Foreground mode
            fuse_args.push_back(argv[i]);
        } else if (strcmp(argv[i], "-d") == 0) {
            // Debug mode
            g_debug_mode = true;
            // Also pass -d to FUSE for its debug output
            fuse_args.push_back(argv[i]);
        } else if (strcmp(argv[i], "-s") == 0) {
            // Single-threaded mode
            fuse_args.push_back(argv[i]);
        } else if (strcmp(argv[i], "--llama-swap") == 0 || strcmp(argv[i], "-ls") == 0) {
            // Generate llama-swap config
            g_generate_llama_swap = true;
        } else if (strcmp(argv[i], "--base-url") == 0) {
            // Set base URL for llama-swap
            if (i + 1 < argc) {
                g_llama_swap_base_url = argv[i+1];
                i++;
            } else {
                std::cerr << "Error: --base-url requires an argument" << std::endl;
                return 1;
            }
        } else if (strncmp(argv[i], "--ollama-dir=", 13) == 0) {
            // Parse ollama directory from --ollama-dir option
            ollama_dirs.push_back(argv[i] + 13);
        } else if (argv[i][0] != '-') {
            // If it's not an option, assume it's an ollama path
            ollama_dirs.push_back(argv[i]);
        } else {
            // Unknown option, assume it's for FUSE
            fuse_args.push_back(argv[i]);
        }
    }
    
    // If no Ollama directories were specified, use the default
    if (ollama_dirs.empty()) {
        ollama_dirs.push_back(DEFAULT_OLLAMA_DIR);
    }
    
    // Set Ollama directories
    g_ollama_dirs = ollama_dirs;
    if (g_debug_mode) {
        std::cout << "Using Ollama directories:" << std::endl;
        for (const auto& dir : g_ollama_dirs) {
            std::cout << "  " << dir << std::endl;
        }
    } else {
        std::cout << "Using " << g_ollama_dirs.size() << " Ollama director" << (g_ollama_dirs.size() == 1 ? "y" : "ies") << std::endl;
    }
    
    // Discover Ollama models
    if (!discover_ollama_models()) {
        std::cerr << "Failed to discover Ollama models. Is Ollama installed?" << std::endl;
        return 1;
    }
    
    // Add default FUSE options if not specified
    bool has_foreground = false;
    bool has_options = false;
    
    for (size_t i = 0; i < fuse_args.size(); i++) {
        if (strcmp(fuse_args[i], "-f") == 0) {
            has_foreground = true;
        } else if (strcmp(fuse_args[i], "-o") == 0) {
            has_options = true;
        }
    }
    
    if (!has_options) {
        fuse_args.push_back(strdup("-o"));
        fuse_args.push_back(strdup("ro,default_permissions"));  // Read-only
    }
    
    // Start filesystem monitoring
    start_filesystem_monitoring();
    
    if (g_debug_mode) {
        std::cout << "Filesystem monitoring started" << std::endl;
    }
    
    // Generate llama-swap config if requested
    if (g_generate_llama_swap) {
        std::string config_path = "llama-swap-config.yaml";
        generate_llama_swap_config(config_path);
    }
    
    // Run FUSE
    int result = fuse_main(fuse_args.size(), fuse_args.data(), &ollama_oper, nullptr);
    
    // Stop filesystem monitoring
    stop_filesystem_monitoring();
    
    return result;
}
