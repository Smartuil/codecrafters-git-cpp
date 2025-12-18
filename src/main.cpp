#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <zlib.h>
#include <openssl/sha.h>

std::string decompressZlib(const std::vector<char>& compressedData)
{
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.avail_in = compressedData.size();
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressedData.data()));

    if (inflateInit(&zs) != Z_OK)
    {
        throw std::runtime_error("Failed to initialize zlib");
    }

    std::string result;
    char buffer[4096];

    int ret;
    do
    {
        zs.avail_out = sizeof(buffer);
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END)
        {
            inflateEnd(&zs);
            throw std::runtime_error("Failed to decompress data");
        }
        result.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return result;
}

std::vector<char> compressZlib(const std::string& data)
{
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;

    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK)
    {
        throw std::runtime_error("Failed to initialize zlib for compression");
    }

    zs.avail_in = data.size();
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));

    std::vector<char> result;
    char buffer[4096];

    int ret;
    do
    {
        zs.avail_out = sizeof(buffer);
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        ret = deflate(&zs, Z_FINISH);
        if (ret == Z_STREAM_ERROR)
        {
            deflateEnd(&zs);
            throw std::runtime_error("Failed to compress data");
        }
        result.insert(result.end(), buffer, buffer + (sizeof(buffer) - zs.avail_out));
    } while (ret != Z_STREAM_END);

    deflateEnd(&zs);
    return result;
}

std::string computeSha1(const std::string& data)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i)
    {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// Write object to .git/objects and return hex SHA
std::string writeObject(const std::string& objectData)
{
    std::string sha1Hex = computeSha1(objectData);
    std::vector<char> compressed = compressZlib(objectData);

    std::string objectDir = ".git/objects/" + sha1Hex.substr(0, 2);
    std::string objectPath = objectDir + "/" + sha1Hex.substr(2);

    std::filesystem::create_directories(objectDir);

    std::ofstream outFile(objectPath, std::ios::binary);
    if (outFile.is_open())
    {
        outFile.write(compressed.data(), compressed.size());
        outFile.close();
    }

    return sha1Hex;
}

// Write a blob for a file and return hex SHA
std::string writeBlob(const std::filesystem::path& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    std::string blobData = "blob " + std::to_string(content.size()) + '\0' + content;
    return writeObject(blobData);
}

// Recursively write a tree for a directory and return hex SHA
std::string writeTree(const std::filesystem::path& dirPath)
{
    struct TreeEntry
    {
        std::string mode;
        std::string name;
        std::string sha;  // raw 20-byte SHA
    };

    std::vector<TreeEntry> entries;

    for (const auto& entry : std::filesystem::directory_iterator(dirPath))
    {
        std::string name = entry.path().filename().string();

        // Skip .git directory
        if (name == ".git") continue;

        TreeEntry treeEntry;
        treeEntry.name = name;

        if (entry.is_directory())
        {
            treeEntry.mode = "40000";
            std::string sha1Hex = writeTree(entry.path());
            // Convert hex SHA to raw bytes
            for (size_t i = 0; i < 40; i += 2)
            {
                treeEntry.sha += static_cast<char>(std::stoi(sha1Hex.substr(i, 2), nullptr, 16));
            }
        }
        else if (entry.is_regular_file())
        {
            treeEntry.mode = "100644";
            std::string sha1Hex = writeBlob(entry.path());
            // Convert hex SHA to raw bytes
            for (size_t i = 0; i < 40; i += 2)
            {
                treeEntry.sha += static_cast<char>(std::stoi(sha1Hex.substr(i, 2), nullptr, 16));
            }
        }
        else
        {
            continue;  // Skip other types
        }

        entries.push_back(treeEntry);
    }

    // Sort entries alphabetically by name
    std::sort(entries.begin(), entries.end(), [](const TreeEntry& a, const TreeEntry& b) {
        return a.name < b.name;
    });

    // Build tree content
    std::string treeContent;
    for (const auto& entry : entries)
    {
        treeContent += entry.mode + " " + entry.name + '\0' + entry.sha;
    }

    // Create tree object with header
    std::string treeData = "tree " + std::to_string(treeContent.size()) + '\0' + treeContent;

    return writeObject(treeData);
}

int main(int argc, char *argv[])
{
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    // You can use print statements as follows for debugging, they'll be visible when running tests.
    std::cerr << "Logs from your program will appear here!\n";

    if (argc < 2) 
    {
        std::cerr << "No command provided.\n";
        return EXIT_FAILURE;
    }
    
    std::string command = argv[1];
    
    if (command == "init") 
    {
        try 
        {
            std::filesystem::create_directory(".git");
            std::filesystem::create_directory(".git/objects");
            std::filesystem::create_directory(".git/refs");
    
            std::ofstream headFile(".git/HEAD");
            if (headFile.is_open()) 
            {
                headFile << "ref: refs/heads/main\n";
                headFile.close();
            } 
            else 
            {
                std::cerr << "Failed to create .git/HEAD file.\n";
                return EXIT_FAILURE;
            }
    
            std::cout << "Initialized git directory\n";
        } 
        catch (const std::filesystem::filesystem_error& e) 
        {
            std::cerr << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    else if (command == "cat-file")
    {
        if (argc < 4 || std::string(argv[2]) != "-p")
        {
            std::cerr << "Usage: cat-file -p <blob_sha>\n";
            return EXIT_FAILURE;
        }

        std::string blobSha = argv[3];
        if (blobSha.length() != 40)
        {
            std::cerr << "Invalid SHA hash length\n";
            return EXIT_FAILURE;
        }

        // Build path: .git/objects/XX/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
        std::string objectPath = ".git/objects/" + blobSha.substr(0, 2) + "/" + blobSha.substr(2);

        // Read compressed file
        std::ifstream file(objectPath, std::ios::binary);
        if (!file.is_open())
        {
            std::cerr << "Failed to open object file: " << objectPath << "\n";
            return EXIT_FAILURE;
        }

        std::vector<char> compressedData((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
        file.close();

        try
        {
            // Decompress the data
            std::string decompressed = decompressZlib(compressedData);

            // Find the null byte that separates header from content
            size_t nullPos = decompressed.find('\0');
            if (nullPos == std::string::npos)
            {
                std::cerr << "Invalid blob format\n";
                return EXIT_FAILURE;
            }

            // Extract and print content (without trailing newline)
            std::string content = decompressed.substr(nullPos + 1);
            std::cout << content;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }
    else if (command == "write-tree")
    {
        try
        {
            std::string treeSha = writeTree(".");
            std::cout << treeSha << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }
    else if (command == "hash-object")
    {
        if (argc < 4 || std::string(argv[2]) != "-w")
        {
            std::cerr << "Usage: hash-object -w <file>\n";
            return EXIT_FAILURE;
        }

        std::string filePath = argv[3];

        // Read file content
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            std::cerr << "Failed to open file: " << filePath << "\n";
            return EXIT_FAILURE;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        // Create blob format: "blob <size>\0<content>"
        std::string blobData = "blob " + std::to_string(content.size()) + '\0' + content;

        // Compute SHA-1 hash
        std::string sha1Hash = computeSha1(blobData);

        // Compress the data
        std::vector<char> compressedData = compressZlib(blobData);

        // Create directory and write file
        std::string objectDir = ".git/objects/" + sha1Hash.substr(0, 2);
        std::string objectPath = objectDir + "/" + sha1Hash.substr(2);

        try
        {
            std::filesystem::create_directories(objectDir);

            std::ofstream outFile(objectPath, std::ios::binary);
            if (!outFile.is_open())
            {
                std::cerr << "Failed to create object file: " << objectPath << "\n";
                return EXIT_FAILURE;
            }

            outFile.write(compressedData.data(), compressedData.size());
            outFile.close();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error writing object: " << e.what() << "\n";
            return EXIT_FAILURE;
        }

        // Print SHA-1 hash to stdout
        std::cout << sha1Hash << "\n";
    }
    else if (command == "ls-tree")
    {
        bool nameOnly = false;
        std::string treeSha;

        // Parse arguments
        for (int i = 2; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--name-only")
            {
                nameOnly = true;
            }
            else
            {
                treeSha = arg;
            }
        }

        if (treeSha.empty() || treeSha.length() != 40)
        {
            std::cerr << "Usage: ls-tree [--name-only] <tree_sha>\n";
            return EXIT_FAILURE;
        }

        // Build path to tree object
        std::string objectPath = ".git/objects/" + treeSha.substr(0, 2) + "/" + treeSha.substr(2);

        // Read compressed file
        std::ifstream file(objectPath, std::ios::binary);
        if (!file.is_open())
        {
            std::cerr << "Failed to open object file: " << objectPath << "\n";
            return EXIT_FAILURE;
        }

        std::vector<char> compressedData((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
        file.close();

        try
        {
            // Decompress the data
            std::string decompressed = decompressZlib(compressedData);

            // Find the null byte that separates header from entries
            size_t nullPos = decompressed.find('\0');
            if (nullPos == std::string::npos)
            {
                std::cerr << "Invalid tree format\n";
                return EXIT_FAILURE;
            }

            // Parse entries: each entry is "<mode> <name>\0<20-byte SHA>"
            size_t pos = nullPos + 1;
            while (pos < decompressed.size())
            {
                // Find space between mode and name
                size_t spacePos = decompressed.find(' ', pos);
                if (spacePos == std::string::npos) break;

                std::string mode = decompressed.substr(pos, spacePos - pos);

                // Find null byte after name
                size_t nameNullPos = decompressed.find('\0', spacePos + 1);
                if (nameNullPos == std::string::npos) break;

                std::string name = decompressed.substr(spacePos + 1, nameNullPos - spacePos - 1);

                // Skip the 20-byte SHA (raw bytes, not hex)
                pos = nameNullPos + 1 + 20;

                if (nameOnly)
                {
                    std::cout << name << "\n";
                }
                else
                {
                    // Convert raw SHA to hex
                    std::ostringstream shaHex;
                    for (int i = 0; i < 20; ++i)
                    {
                        shaHex << std::hex << std::setfill('0') << std::setw(2)
                               << static_cast<int>(static_cast<unsigned char>(decompressed[nameNullPos + 1 + i]));
                    }

                    // Determine type based on mode
                    std::string type = (mode == "40000") ? "tree" : "blob";

                    // Pad mode to 6 digits
                    while (mode.length() < 6) mode = "0" + mode;

                    std::cout << mode << " " << type << " " << shaHex.str() << "\t" << name << "\n";
                }
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }
    else
    {
        std::cerr << "Unknown command " << command << '\n';
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}
