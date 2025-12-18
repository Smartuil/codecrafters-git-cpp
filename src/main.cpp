#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <zlib.h>

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
    else
    {
        std::cerr << "Unknown command " << command << '\n';
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}
