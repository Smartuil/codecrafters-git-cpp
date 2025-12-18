#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <map>
#include <zlib.h>
#include <openssl/sha.h>
#include <curl/curl.h>

// Global base path for .git directory
std::string g_gitDir = ".git";

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

    std::string objectDir = g_gitDir + "/objects/" + sha1Hex.substr(0, 2);
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

// Write object with known SHA
void writeObjectWithSha(const std::string& objectData, const std::string& sha1Hex)
{
    std::string objectDir = g_gitDir + "/objects/" + sha1Hex.substr(0, 2);
    std::string objectPath = objectDir + "/" + sha1Hex.substr(2);

    if (std::filesystem::exists(objectPath)) return;

    std::vector<char> compressed = compressZlib(objectData);
    std::filesystem::create_directories(objectDir);

    std::ofstream outFile(objectPath, std::ios::binary);
    if (outFile.is_open())
    {
        outFile.write(compressed.data(), compressed.size());
        outFile.close();
    }
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

// ==================== Clone Implementation ====================

size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* output)
{
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string shaToHex(const unsigned char* sha)
{
    std::ostringstream oss;
    for (int i = 0; i < 20; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(sha[i]);
    return oss.str();
}

std::string shaToHex(const std::string& sha)
{
    return shaToHex(reinterpret_cast<const unsigned char*>(sha.data()));
}

std::string httpGet(const std::string& url)
{
    CURL* curl = curl_easy_init();
    std::string response;
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "git/codecrafters");
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK)
            throw std::runtime_error("HTTP GET failed");
    }
    return response;
}

std::string httpPost(const std::string& url, const std::string& data, const std::string& contentType)
{
    CURL* curl = curl_easy_init();
    std::string response;
    if (curl)
    {
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Content-Type: " + contentType).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "git/codecrafters");
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK)
            throw std::runtime_error("HTTP POST failed");
    }
    return response;
}

std::vector<std::string> parsePktLines(const std::string& data)
{
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos + 4 <= data.size())
    {
        std::string lenHex = data.substr(pos, 4);
        int len = std::stoi(lenHex, nullptr, 16);
        if (len == 0) { pos += 4; continue; }
        if (len < 4 || pos + len > data.size()) break;
        std::string line = data.substr(pos + 4, len - 4);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        lines.push_back(line);
        pos += len;
    }
    return lines;
}

std::string createPktLine(const std::string& data)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(4) << (data.size() + 4);
    return oss.str() + data;
}

std::pair<std::string, size_t> decompressZlibStream(const std::string& data, size_t offset)
{
    z_stream zs = {};
    zs.avail_in = data.size() - offset;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + offset));
    if (inflateInit(&zs) != Z_OK)
        throw std::runtime_error("Failed to init zlib");

    std::string result;
    char buffer[4096];
    int ret;
    do
    {
        zs.avail_out = sizeof(buffer);
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
        {
            inflateEnd(&zs);
            throw std::runtime_error("Decompress failed");
        }
        result.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    size_t consumed = zs.total_in;
    inflateEnd(&zs);
    return {result, consumed};
}

enum PackObjType { OBJ_COMMIT=1, OBJ_TREE=2, OBJ_BLOB=3, OBJ_TAG=4, OBJ_OFS_DELTA=6, OBJ_REF_DELTA=7 };

std::string typeToString(int t)
{
    switch(t) {
        case OBJ_COMMIT: return "commit";
        case OBJ_TREE: return "tree";
        case OBJ_BLOB: return "blob";
        case OBJ_TAG: return "tag";
        default: return "unknown";
    }
}

std::string applyDelta(const std::string& base, const std::string& delta)
{
    size_t pos = 0;
    // Read source size
    size_t srcSize = 0, shift = 0;
    while (pos < delta.size()) {
        unsigned char b = delta[pos++];
        srcSize |= (b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    // Read target size
    size_t targetSize = 0;
    shift = 0;
    while (pos < delta.size()) {
        unsigned char b = delta[pos++];
        targetSize |= (b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }

    std::string result;
    result.reserve(targetSize);

    while (pos < delta.size())
    {
        unsigned char cmd = delta[pos++];
        if (cmd & 0x80)
        {
            size_t copyOff = 0, copySize = 0;
            if (cmd & 0x01) copyOff |= static_cast<unsigned char>(delta[pos++]);
            if (cmd & 0x02) copyOff |= static_cast<unsigned char>(delta[pos++]) << 8;
            if (cmd & 0x04) copyOff |= static_cast<unsigned char>(delta[pos++]) << 16;
            if (cmd & 0x08) copyOff |= static_cast<unsigned char>(delta[pos++]) << 24;
            if (cmd & 0x10) copySize |= static_cast<unsigned char>(delta[pos++]);
            if (cmd & 0x20) copySize |= static_cast<unsigned char>(delta[pos++]) << 8;
            if (cmd & 0x40) copySize |= static_cast<unsigned char>(delta[pos++]) << 16;
            if (copySize == 0) copySize = 0x10000;
            result.append(base.substr(copyOff, copySize));
        }
        else if (cmd > 0)
        {
            result.append(delta.substr(pos, cmd));
            pos += cmd;
        }
    }
    return result;
}

struct PackObject {
    int type;
    std::string data;
    std::string sha;
    size_t offset;
    std::string baseSha;
    size_t baseOffset = 0;
};

std::map<std::string, PackObject> parsePackfile(const std::string& packData)
{
    std::map<std::string, PackObject> objects;
    std::map<size_t, std::string> offsetToSha;

    if (packData.substr(0, 4) != "PACK")
        throw std::runtime_error("Invalid packfile");

    size_t pos = 8;
    uint32_t numObjects = (static_cast<unsigned char>(packData[pos]) << 24) |
                          (static_cast<unsigned char>(packData[pos+1]) << 16) |
                          (static_cast<unsigned char>(packData[pos+2]) << 8) |
                          static_cast<unsigned char>(packData[pos+3]);
    pos += 4;

    std::vector<PackObject> tempObjs;

    for (uint32_t i = 0; i < numObjects; ++i)
    {
        size_t objOffset = pos;
        PackObject obj;
        obj.offset = objOffset;

        unsigned char b = packData[pos++];
        obj.type = (b >> 4) & 0x07;
        size_t size = b & 0x0f;
        int shift = 4;
        while (b & 0x80) {
            b = packData[pos++];
            size |= (b & 0x7f) << shift;
            shift += 7;
        }

        if (obj.type == OBJ_OFS_DELTA) {
            b = packData[pos++];
            size_t negOff = b & 0x7f;
            while (b & 0x80) {
                b = packData[pos++];
                negOff = ((negOff + 1) << 7) | (b & 0x7f);
            }
            obj.baseOffset = objOffset - negOff;
        } else if (obj.type == OBJ_REF_DELTA) {
            obj.baseSha = shaToHex(reinterpret_cast<const unsigned char*>(packData.data() + pos));
            pos += 20;
        }

        auto [decompressed, consumed] = decompressZlibStream(packData, pos);
        obj.data = decompressed;
        pos += consumed;
        tempObjs.push_back(obj);
    }

    // First pass: non-delta objects
    for (auto& obj : tempObjs) {
        if (obj.type != OBJ_OFS_DELTA && obj.type != OBJ_REF_DELTA) {
            std::string header = typeToString(obj.type) + " " + std::to_string(obj.data.size()) + '\0';
            obj.sha = computeSha1(header + obj.data);
            objects[obj.sha] = obj;
            offsetToSha[obj.offset] = obj.sha;
        }
    }

    // Resolve deltas
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto& obj : tempObjs) {
            if (!obj.sha.empty()) continue;

            std::string baseSha;
            if (obj.type == OBJ_OFS_DELTA) {
                auto it = offsetToSha.find(obj.baseOffset);
                if (it != offsetToSha.end()) baseSha = it->second;
            } else if (obj.type == OBJ_REF_DELTA) {
                baseSha = obj.baseSha;
            }
            if (baseSha.empty()) continue;

            auto baseIt = objects.find(baseSha);
            if (baseIt == objects.end()) continue;

            obj.data = applyDelta(baseIt->second.data, obj.data);
            obj.type = baseIt->second.type;
            std::string header = typeToString(obj.type) + " " + std::to_string(obj.data.size()) + '\0';
            obj.sha = computeSha1(header + obj.data);
            objects[obj.sha] = obj;
            offsetToSha[obj.offset] = obj.sha;
            progress = true;
        }
    }
    return objects;
}

std::pair<std::string, std::string> readObject(const std::string& sha)
{
    std::string path = g_gitDir + "/objects/" + sha.substr(0,2) + "/" + sha.substr(2);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Object not found: " + sha);

    std::vector<char> compressed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string decompressed = decompressZlib(compressed);

    size_t nullPos = decompressed.find('\0');
    std::string header = decompressed.substr(0, nullPos);
    std::string content = decompressed.substr(nullPos + 1);
    std::string type = header.substr(0, header.find(' '));
    return {type, content};
}

void checkoutTree(const std::string& treeSha, const std::filesystem::path& dir)
{
    auto [type, content] = readObject(treeSha);
    if (type != "tree") throw std::runtime_error("Expected tree");

    size_t pos = 0;
    while (pos < content.size())
    {
        size_t spacePos = content.find(' ', pos);
        std::string mode = content.substr(pos, spacePos - pos);
        pos = spacePos + 1;

        size_t nullPos = content.find('\0', pos);
        std::string name = content.substr(pos, nullPos - pos);
        pos = nullPos + 1;

        std::string sha = shaToHex(reinterpret_cast<const unsigned char*>(content.data() + pos));
        pos += 20;

        std::filesystem::path entryPath = dir / name;

        if (mode == "40000") {
            std::filesystem::create_directories(entryPath);
            checkoutTree(sha, entryPath);
        } else {
            auto [objType, fileContent] = readObject(sha);
            std::ofstream out(entryPath, std::ios::binary);
            out.write(fileContent.data(), fileContent.size());
        }
    }
}

void cloneRepository(const std::string& repoUrl, const std::string& targetDir)
{
    std::filesystem::create_directories(targetDir);
    g_gitDir = targetDir + "/.git";
    std::filesystem::create_directories(g_gitDir + "/objects");
    std::filesystem::create_directories(g_gitDir + "/refs/heads");

    // Discover refs
    std::string refsUrl = repoUrl + "/info/refs?service=git-upload-pack";
    std::string refsResp = httpGet(refsUrl);
    auto lines = parsePktLines(refsResp);

    std::string headSha, headRef;
    for (const auto& line : lines) {
        if (line.find("# service=") != std::string::npos) continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string sha = line.substr(0, sp);
        if (sha.length() != 40) continue;
        std::string refPart = line.substr(sp + 1);
        size_t np = refPart.find('\0');
        std::string ref = (np != std::string::npos) ? refPart.substr(0, np) : refPart;

        if (ref == "HEAD") headSha = sha;
        else if (ref == "refs/heads/master" || ref == "refs/heads/main") {
            if (headSha.empty()) headSha = sha;
            headRef = ref;
        }
    }
    if (headSha.empty()) throw std::runtime_error("No HEAD found");

    // Request packfile
    std::string req = createPktLine("want " + headSha + "\n") + "0000" + createPktLine("done\n");
    std::string packResp = httpPost(repoUrl + "/git-upload-pack", req, "application/x-git-upload-pack-request");

    size_t packPos = packResp.find("PACK");
    if (packPos == std::string::npos) throw std::runtime_error("No packfile");
    std::string packData = packResp.substr(packPos);

    // Parse and write objects
    auto objects = parsePackfile(packData);
    for (const auto& [sha, obj] : objects) {
        std::string header = typeToString(obj.type) + " " + std::to_string(obj.data.size()) + '\0';
        writeObjectWithSha(header + obj.data, sha);
    }

    // Write HEAD
    std::ofstream headFile(g_gitDir + "/HEAD");
    if (headRef.empty()) {
        headFile << headSha << "\n";
    } else {
        headFile << "ref: " << headRef << "\n";
        std::string refPath = g_gitDir + "/" + headRef;
        std::filesystem::create_directories(std::filesystem::path(refPath).parent_path());
        std::ofstream(refPath) << headSha << "\n";
    }

    // Checkout
    auto [commitType, commitContent] = readObject(headSha);
    std::istringstream iss(commitContent);
    std::string line, treeSha;
    while (std::getline(iss, line)) {
        if (line.substr(0, 5) == "tree ") { treeSha = line.substr(5); break; }
    }
    if (treeSha.empty()) throw std::runtime_error("No tree in commit");
    checkoutTree(treeSha, targetDir);
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
    else if (command == "commit-tree")
    {
        // Parse arguments: commit-tree <tree_sha> -p <parent_sha> -m <message>
        if (argc < 6)
        {
            std::cerr << "Usage: commit-tree <tree_sha> -p <parent_sha> -m <message>\n";
            return EXIT_FAILURE;
        }

        std::string treeSha = argv[2];
        std::string parentSha;
        std::string message;

        for (int i = 3; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "-p" && i + 1 < argc)
            {
                parentSha = argv[++i];
            }
            else if (arg == "-m" && i + 1 < argc)
            {
                message = argv[++i];
            }
        }

        if (treeSha.length() != 40 || parentSha.length() != 40 || message.empty())
        {
            std::cerr << "Invalid arguments\n";
            return EXIT_FAILURE;
        }

        // Build commit content
        std::string commitContent;
        commitContent += "tree " + treeSha + "\n";
        commitContent += "parent " + parentSha + "\n";
        commitContent += "author John Doe <john@example.com> 1234567890 +0000\n";
        commitContent += "committer John Doe <john@example.com> 1234567890 +0000\n";
        commitContent += "\n";
        commitContent += message + "\n";

        // Create commit object with header
        std::string commitData = "commit " + std::to_string(commitContent.size()) + '\0' + commitContent;

        try
        {
            std::string commitSha = writeObject(commitData);
            std::cout << commitSha << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }
    else if (command == "clone")
    {
        if (argc < 4)
        {
            std::cerr << "Usage: clone <repo_url> <target_dir>\n";
            return EXIT_FAILURE;
        }

        std::string repoUrl = argv[2];
        std::string targetDir = argv[3];

        try
        {
            curl_global_init(CURL_GLOBAL_DEFAULT);
            cloneRepository(repoUrl, targetDir);
            curl_global_cleanup();
        }
        catch (const std::exception& e)
        {
            curl_global_cleanup();
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
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
