#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include "json.hpp"

// For path buffer sizes
#include <limits.h>
#include <set>

// For inotify
#include <sys/inotify.h>
#include <poll.h>

// Define the default Ollama storage directory
#define DEFAULT_OLLAMA_DIR (getenv("OLLAMA_HOME") ? std::string(getenv("OLLAMA_HOME")) : std::string(getenv("HOME")) + "/.ollama")

using json = nlohmann::json;

// Data structures to represent the Ollama filesystem
struct ModelInfo {
    std::string name;           // Model name (e.g., "llama3.2")
    std::string version;        // Model version (e.g., "latest")
    std::string gguf_path;      // Path to the GGUF file in the Ollama filesystem
    std::string modelfile_path; // Path to the ModelFile in the Ollama filesystem
    std::string manifest_path;  // Path to the manifest file
    json manifest_data;         // Parsed manifest data
};

// Global variables
static std::string g_ollama_dir;
static std::vector<ModelInfo> g_models;
static std::map<std::string, std::string> g_file_mappings; // Maps virtual paths to real paths

// Filesystem monitoring variables
static std::atomic<bool> g_monitor_running(false);
static std::thread g_monitor_thread;
static std::mutex g_models_mutex; // Mutex to protect g_models and g_file_mappings
static int g_inotify_fd = -1;
static int g_watch_descriptor = -1;

// Helper functions
static std::string get_filename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

static std::string get_dirname(const std::string& path) {
    size_t pos = path.find_last_of('/');
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

// Function to initialize inotify monitoring
static bool init_filesystem_monitoring() {
    // Initialize inotify
    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd == -1) {
        std::cerr << "Failed to initialize inotify: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Add watch for the manifests directory
    std::string manifests_dir = g_ollama_dir + "/models/manifests";
    g_watch_descriptor = inotify_add_watch(g_inotify_fd, manifests_dir.c_str(), 
                                          IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
    if (g_watch_descriptor == -1) {
        std::cerr << "Failed to add watch for directory: " << manifests_dir << ": " << strerror(errno) << std::endl;
        close(g_inotify_fd);
        g_inotify_fd = -1;
        return false;
    }
    
    return true;
}

// Function to stop filesystem monitoring
static void stop_filesystem_monitoring() {
    if (g_monitor_running.exchange(false)) {
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

// Function to monitor filesystem changes
static void monitor_filesystem_changes() {
    const size_t BUF_LEN = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
    char buffer[BUF_LEN];
    struct pollfd pfd = { g_inotify_fd, POLLIN, 0 };
    
    g_monitor_running = true;
    
    while (g_monitor_running) {
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
                std::cout << "Changes detected in Ollama models directory, refreshing..." << std::endl;
                discover_ollama_models();
            }
        }
    }
}

// Function to start filesystem monitoring in a separate thread
static void start_filesystem_monitoring() {
    if (init_filesystem_monitoring()) {
        g_monitor_thread = std::thread(monitor_filesystem_changes);
        std::cout << "Filesystem monitoring started" << std::endl;
    } else {
        std::cerr << "Failed to start filesystem monitoring" << std::endl;
    }
}

// Function to discover Ollama models
static bool discover_ollama_models() {
    // Lock the mutex to ensure thread safety
    std::lock_guard<std::mutex> lock(g_models_mutex);
    
    // Clear existing models and file mappings
    g_models.clear();
    g_file_mappings.clear();
    
    std::string manifests_dir = g_ollama_dir + "/models/manifests";
    DIR* dir = opendir(manifests_dir.c_str());
    if (!dir) {
        std::cerr << "Failed to open Ollama manifests directory: " << manifests_dir << std::endl;
        return false;
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
                        model.manifest_path = manifest_path;
                        model.manifest_data = manifest_json;
                        
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
                                        model.gguf_path = g_ollama_dir + "/models/blobs/" + digest_filename;
                                        
                                        // Check if the file exists
                                        struct stat st;
                                        if (stat(model.gguf_path.c_str(), &st) != 0) {
                                            std::cerr << "Warning: GGUF file not found at " << model.gguf_path << std::endl;
                                        } else {
                                            std::cout << "Found GGUF file at " << model.gguf_path << " (" << (st.st_size / (1024*1024)) << " MB)" << std::endl;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        
                        // Add the model to our list
                        g_models.push_back(model);
                        
                        // Create file mappings
                        std::string virtual_gguf_path = "/" + model_name + "/" + version + ".gguf";
                        std::string virtual_modelfile_path = "/" + model_name + "/" + version + ".modelfile";
                        
                        if (!model.gguf_path.empty()) {
                            g_file_mappings[virtual_gguf_path] = model.gguf_path;
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
                            std::string config_path = g_ollama_dir + "/models/blobs/" + config_filename;
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
                                    std::string blob_path = g_ollama_dir + "/models/blobs/" + digest_filename;
                                    
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
                                                if (params_json.contains("stop") && params_json["stop"].is_array()) {
                                                    modelfile_content += "PARAMETER stop \"";
                                                    for (size_t i = 0; i < params_json["stop"].size(); ++i) {
                                                        if (i > 0) modelfile_content += ",";
                                                        modelfile_content += params_json["stop"][i].get<std::string>();
                                                    }
                                                    modelfile_content += "\"\n";
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

    std::cout << "Discovered " << g_models.size() << " Ollama models" << std::endl;
    for (const auto& model : g_models) {
        std::cout << "Model: " << model.name << ", Version: " << model.version << std::endl;
    }
    
    // Print file mappings for debugging
    std::cout << "\nFile mappings:" << std::endl;
    for (const auto& mapping : g_file_mappings) {
        std::cout << "Virtual path: " << mapping.first << " -> Real path: " << mapping.second << std::endl;
    }

    return !g_models.empty();
}

// FUSE operations
static int ollama_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    // Lock the mutex to ensure thread safety
    std::lock_guard<std::mutex> lock(g_models_mutex);

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    // Check if this is a model directory
    for (const auto& model : g_models) {
        std::string model_dir = "/" + model.name;
        if (strcmp(path, model_dir.c_str()) == 0) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            return 0;
        }
    }

    // Check if this is a file in our mappings
    if (g_file_mappings.find(path) != g_file_mappings.end()) {
        const std::string& real_path = g_file_mappings[path];
        struct stat real_stat;
        if (stat(real_path.c_str(), &real_stat) == 0) {
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

static int ollama_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));

    if (strcmp(path, "/") == 0) {
        // Root directory - list all model names
        std::set<std::string> unique_models;
        for (const auto& model : g_models) {
            unique_models.insert(model.name);
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
            if (model.name == model_name) {
                std::string gguf_file = model.version + ".gguf";
                std::string modelfile = model.version + ".modelfile";
                
                filler(buf, gguf_file.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
                filler(buf, modelfile.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
            }
        }
        
        return 0;
    }

    return -ENOENT;
}

static int ollama_open(const char* path, struct fuse_file_info* fi) {
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

static const struct fuse_operations ollama_oper = {
    /* getattr */ ollama_getattr,
    /* readlink */ nullptr,
    /* mknod */ nullptr,
    /* mkdir */ nullptr,
    /* unlink */ nullptr,
    /* rmdir */ nullptr,
    /* symlink */ nullptr,
    /* rename */ nullptr,
    /* link */ nullptr,
    /* chmod */ nullptr,
    /* chown */ nullptr,
    /* truncate */ nullptr,
    /* open */ ollama_open,
    /* read */ ollama_read,
    /* write */ nullptr,
    /* statfs */ nullptr,
    /* flush */ nullptr,
    /* release */ nullptr,
    /* fsync */ nullptr,
    /* setxattr */ nullptr,
    /* getxattr */ nullptr,
    /* listxattr */ nullptr,
    /* removexattr */ nullptr,
    /* opendir */ nullptr,
    /* readdir */ ollama_readdir,
    /* releasedir */ nullptr,
    /* fsyncdir */ nullptr,
    /* init */ nullptr,
    /* destroy */ nullptr,
    /* access */ nullptr,
    /* create */ nullptr,
    /* lock */ nullptr,
    /* utimens */ nullptr,
    /* bmap */ nullptr,
    /* ioctl */ nullptr,
    /* poll */ nullptr,
    /* write_buf */ nullptr,
    /* read_buf */ nullptr,
    /* flock */ nullptr,
    /* fallocate */ nullptr,
    /* copy_file_range */ nullptr,
};

int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <mountpoint> [ollama_path]" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  ollama_path    Path to Ollama directory (default: $OLLAMA_HOME or $HOME/.ollama)" << std::endl;
        std::cerr << "  -f             Run in foreground (default)" << std::endl;
        std::cerr << "  -o opt,[opt]   FUSE mount options" << std::endl;
        std::cerr << "\nEnvironment Variables:" << std::endl;
        std::cerr << "  OLLAMA_HOME    Path to Ollama directory (used if ollama_path not specified)" << std::endl;
        return 1;
    }
    
    // Prepare FUSE arguments
    std::vector<char*> fuse_args;
    fuse_args.push_back(argv[0]);  // Program name
    fuse_args.push_back(argv[1]);  // Mount point
    
    // Process our own arguments
    std::string ollama_dir = DEFAULT_OLLAMA_DIR;
    bool ollama_dir_set = false;
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "-f") == 0 || 
            strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "-s") == 0) {
            // Pass through FUSE options
            fuse_args.push_back(argv[i]);
            if (i + 1 < argc && strcmp(argv[i], "-o") == 0) {
                fuse_args.push_back(argv[i+1]);
                i++;
            }
        } else if (strncmp(argv[i], "--ollama-dir=", 13) == 0) {
            // Parse ollama directory from --ollama-dir option
            ollama_dir = argv[i] + 13;
            ollama_dir_set = true;
        } else if (!ollama_dir_set && argv[i][0] != '-') {
            // If it's not an option and ollama_dir is not set yet, assume it's the ollama path
            ollama_dir = argv[i];
            ollama_dir_set = true;
        } else {
            // Unknown option, assume it's for FUSE
            fuse_args.push_back(argv[i]);
        }
    }
    
    // Set Ollama directory
    g_ollama_dir = ollama_dir;
    std::cout << "Using Ollama directory: " << g_ollama_dir << std::endl;
    
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
    
    if (!has_foreground) {
        fuse_args.push_back(strdup("-f"));  // Foreground
    }
    
    if (!has_options) {
        fuse_args.push_back(strdup("-o"));
        fuse_args.push_back(strdup("ro,default_permissions"));  // Read-only
    }
    
    // Start filesystem monitoring
    start_filesystem_monitoring();
    
    // Run FUSE
    int result = fuse_main(fuse_args.size(), fuse_args.data(), &ollama_oper, nullptr);
    
    // Stop filesystem monitoring
    stop_filesystem_monitoring();
    
    return result;
}
