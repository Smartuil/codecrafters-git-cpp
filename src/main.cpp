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
//
// Git Clone 实现概述:
// ====================
// Git clone 使用 Smart HTTP 协议从远程仓库获取所有对象并检出工作目录。
//
// 整体流程:
// 1. 初始化本地 .git 目录结构
// 2. 通过 HTTP GET 请求 /info/refs?service=git-upload-pack 获取远程引用列表
// 3. 解析 pkt-line 格式的响应，提取 HEAD 和分支的 SHA
// 4. 通过 HTTP POST 请求 /git-upload-pack 发送 "want" 请求获取 packfile
// 5. 解析 packfile，处理普通对象和 delta 对象
// 6. 将所有对象写入 .git/objects/
// 7. 设置 HEAD 和 refs
// 8. 从 commit -> tree 检出文件到工作目录
//
// 关键数据格式:
// - pkt-line: 4字节十六进制长度前缀 + 数据，"0000" 表示结束
// - packfile: "PACK" + 版本(4字节) + 对象数(4字节) + 压缩对象序列 + SHA校验
// - delta: 基于另一个对象的差异编码，需要解析指令重建完整对象

/**
 * CURL 写回调函数
 * 当 CURL 接收到 HTTP 响应数据时调用此函数
 * @param contents 接收到的数据指针
 * @param size 每个数据单元的大小（通常为1）
 * @param nmemb 数据单元的数量
 * @param output 用于存储响应的字符串指针
 * @return 实际处理的字节数，必须返回 size*nmemb 否则 CURL 认为出错
 */
size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* output)
{
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

/**
 * 将 20 字节的原始 SHA-1 转换为 40 字符的十六进制字符串
 * 
 * Git 内部存储使用 20 字节原始格式（如 tree 条目中的 SHA）
 * 但文件路径和显示使用 40 字符十六进制格式
 * 
 * @param sha 指向 20 字节原始 SHA 数据的指针
 * @return 40 字符的十六进制 SHA 字符串
 * 
 * 示例: [0xab, 0xcd, 0x12, ...] -> "abcd12..."
 */
std::string shaToHex(const unsigned char* sha)
{
    std::ostringstream oss;
    for (int i = 0; i < 20; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(sha[i]);
    return oss.str();
}

/**
 * shaToHex 的重载版本，接受 std::string 参数
 */
std::string shaToHex(const std::string& sha)
{
    return shaToHex(reinterpret_cast<const unsigned char*>(sha.data()));
}

/**
 * 发送 HTTP GET 请求
 * 
 * 用于获取远程仓库的引用列表 (refs)
 * 请求 URL: {repoUrl}/info/refs?service=git-upload-pack
 * 
 * @param url 请求的完整 URL
 * @return 响应体内容
 * @throws std::runtime_error 如果请求失败
 * 
 * 响应格式 (pkt-line):
 * 001e# service=git-upload-pack\n
 * 0000
 * 00a8<sha> HEAD\0<capabilities>\n
 * 003f<sha> refs/heads/master\n
 * 0000
 */
std::string httpGet(const std::string& url)
{
    CURL* curl = curl_easy_init();
    std::string response;
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // 跟随重定向
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "git/codecrafters");
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK)
            throw std::runtime_error("HTTP GET failed");
    }
    return response;
}

/**
 * 发送 HTTP POST 请求
 * 
 * 用于向远程仓库请求 packfile
 * 请求 URL: {repoUrl}/git-upload-pack
 * Content-Type: application/x-git-upload-pack-request
 * 
 * @param url 请求的完整 URL
 * @param data 请求体数据 (pkt-line 格式的 want 请求)
 * @param contentType Content-Type 头
 * @return 响应体内容 (包含 packfile)
 * @throws std::runtime_error 如果请求失败
 * 
 * 请求体格式:
 * 0032want <sha>\n
 * 0000
 * 0009done\n
 * 
 * 响应格式:
 * 0008NAK\n
 * PACK<version:4><count:4><objects...><checksum:20>
 */
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

/**
 * 解析 pkt-line 格式的数据
 * 
 * pkt-line 是 Git 协议的基本数据格式:
 * - 每行以 4 字节十六进制长度前缀开始 (包含前缀本身的长度)
 * - "0000" 是特殊的 flush 包，表示一个段落结束
 * - 长度为 0001-0003 是无效的（因为至少需要 4 字节的长度前缀）
 * 
 * @param data 原始 pkt-line 数据
 * @return 解析后的行列表（不含长度前缀和尾部换行符）
 * 
 * 示例输入: "000bHello\n0000"
 * 解析过程:
 *   "000b" -> 长度 11 (0x000b)
 *   数据: "Hello\n" (11-4=7 字节)
 *   "0000" -> flush 包，跳过
 * 输出: ["Hello"]
 */
std::vector<std::string> parsePktLines(const std::string& data)
{
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos + 4 <= data.size())
    {
        // 读取 4 字节十六进制长度
        std::string lenHex = data.substr(pos, 4);
        int len = std::stoi(lenHex, nullptr, 16);
        
        // len=0 是 flush 包，跳过
        if (len == 0) { pos += 4; continue; }
        
        // 长度无效或超出数据范围
        if (len < 4 || pos + len > data.size()) break;
        
        // 提取数据部分 (不含 4 字节长度前缀)
        std::string line = data.substr(pos + 4, len - 4);
        
        // 移除尾部换行符
        if (!line.empty() && line.back() == '\n') line.pop_back();
        
        lines.push_back(line);
        pos += len;
    }
    return lines;
}

/**
 * 创建 pkt-line 格式的数据包
 * 
 * @param data 要发送的数据内容
 * @return pkt-line 格式的字符串 (4字节十六进制长度 + 数据)
 * 
 * 示例: "want abc\n" -> "000dwant abc\n"
 *       长度 = 9 + 4 = 13 = 0x000d
 */
std::string createPktLine(const std::string& data)
{
    std::ostringstream oss;
    // 长度包含 4 字节前缀本身
    oss << std::hex << std::setfill('0') << std::setw(4) << (data.size() + 4);
    return oss.str() + data;
}

/**
 * 从指定偏移位置解压 zlib 流
 * 
 * packfile 中的每个对象都是独立的 zlib 压缩流。
 * 此函数从给定偏移开始解压，并返回解压后的数据和消耗的字节数。
 * 
 * @param data 包含压缩数据的完整字符串
 * @param offset 开始解压的偏移位置
 * @return pair<解压后的数据, 消耗的压缩字节数>
 * 
 * 注意: 返回的 consumed 字节数用于确定下一个对象在 packfile 中的位置
 */
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
        // Z_BUF_ERROR 在某些情况下是正常的，继续处理
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
        {
            inflateEnd(&zs);
            throw std::runtime_error("Decompress failed");
        }
        result.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    size_t consumed = zs.total_in;  // 实际消耗的压缩字节数
    inflateEnd(&zs);
    return {result, consumed};
}

/**
 * Packfile 对象类型枚举
 * 
 * Git packfile 中的对象类型:
 * - OBJ_COMMIT (1): 提交对象
 * - OBJ_TREE (2): 树对象（目录结构）
 * - OBJ_BLOB (3): 文件内容对象
 * - OBJ_TAG (4): 标签对象
 * - OBJ_OFS_DELTA (6): 基于偏移的 delta 对象（引用同一 packfile 中的另一个对象）
 * - OBJ_REF_DELTA (7): 基于 SHA 的 delta 对象（引用任意对象的 SHA）
 * 
 * 注意: 类型 5 未使用
 */
enum PackObjType { OBJ_COMMIT=1, OBJ_TREE=2, OBJ_BLOB=3, OBJ_TAG=4, OBJ_OFS_DELTA=6, OBJ_REF_DELTA=7 };

/**
 * 将对象类型数字转换为字符串
 * 用于构建 Git 对象头 (如 "blob 123\0")
 */
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

/**
 * 应用 delta 指令重建完整对象
 * 
 * Delta 编码是 Git 用于压缩相似对象的技术。
 * delta 数据包含一系列指令，用于从基础对象重建目标对象。
 * 
 * Delta 格式:
 * 1. 源对象大小 (变长编码)
 * 2. 目标对象大小 (变长编码)
 * 3. 指令序列:
 *    - 复制指令 (最高位=1): 从基础对象复制数据
 *      格式: 1oooossss [offset bytes] [size bytes]
 *      - oooo: 指示哪些 offset 字节存在 (bit 0-3)
 *      - ssss: 指示哪些 size 字节存在 (bit 4-6)
 *    - 插入指令 (最高位=0): 直接插入后续数据
 *      格式: 0nnnnnnn <n bytes of data>
 *      - nnnnnnn: 要插入的字节数 (1-127)
 * 
 * @param base 基础对象的内容
 * @param delta delta 指令数据
 * @return 重建后的完整对象内容
 * 
 * 示例:
 * base = "Hello World"
 * delta 指令: 复制 base[0:5], 插入 " Git", 复制 base[5:11]
 * result = "Hello Git World"
 */
std::string applyDelta(const std::string& base, const std::string& delta)
{
    size_t pos = 0;
    
    // 读取源对象大小 (变长编码: 每字节低7位是数据，最高位表示是否继续)
    size_t srcSize = 0, shift = 0;
    while (pos < delta.size()) {
        unsigned char b = delta[pos++];
        srcSize |= (b & 0x7f) << shift;  // 取低 7 位
        shift += 7;
        if (!(b & 0x80)) break;  // 最高位为 0 表示结束
    }
    
    // 读取目标对象大小 (同样的变长编码)
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

    // 处理指令序列
    while (pos < delta.size())
    {
        unsigned char cmd = delta[pos++];
        
        if (cmd & 0x80)
        {
            // 复制指令: 从基础对象复制数据
            // cmd 的低 4 位指示 offset 的哪些字节存在
            // cmd 的 bit 4-6 指示 size 的哪些字节存在
            size_t copyOff = 0, copySize = 0;
            
            // 读取 offset (最多 4 字节，小端序)
            if (cmd & 0x01) copyOff |= static_cast<unsigned char>(delta[pos++]);
            if (cmd & 0x02) copyOff |= static_cast<unsigned char>(delta[pos++]) << 8;
            if (cmd & 0x04) copyOff |= static_cast<unsigned char>(delta[pos++]) << 16;
            if (cmd & 0x08) copyOff |= static_cast<unsigned char>(delta[pos++]) << 24;
            
            // 读取 size (最多 3 字节，小端序)
            if (cmd & 0x10) copySize |= static_cast<unsigned char>(delta[pos++]);
            if (cmd & 0x20) copySize |= static_cast<unsigned char>(delta[pos++]) << 8;
            if (cmd & 0x40) copySize |= static_cast<unsigned char>(delta[pos++]) << 16;
            
            // size 为 0 表示 0x10000 (65536)
            if (copySize == 0) copySize = 0x10000;
            
            // 从基础对象复制指定范围的数据
            result.append(base.substr(copyOff, copySize));
        }
        else if (cmd > 0)
        {
            // 插入指令: cmd 的值就是要插入的字节数
            result.append(delta.substr(pos, cmd));
            pos += cmd;
        }
        // cmd == 0 是保留的，不应该出现
    }
    return result;
}

/**
 * Packfile 中的对象结构
 * 
 * 用于在解析过程中存储对象信息
 */
struct PackObject {
    int type;              // 对象类型 (1-4 或 6-7)
    std::string data;      // 对象内容 (解压后，对于 delta 对象是解析后的完整内容)
    std::string sha;       // 对象的 SHA-1 哈希 (40字符十六进制)
    size_t offset;         // 对象在 packfile 中的起始偏移
    std::string baseSha;   // REF_DELTA: 基础对象的 SHA
    size_t baseOffset = 0; // OFS_DELTA: 基础对象的偏移
};

/**
 * 解析 packfile 并返回所有对象
 * 
 * Packfile 格式:
 * ┌─────────────────────────────────────────────────────────────┐
 * │ "PACK" (4 bytes)                                            │
 * │ Version (4 bytes, big-endian, 通常是 2)                      │
 * │ Object count (4 bytes, big-endian)                          │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Object 1:                                                   │
 * │   ┌─ Type+Size (变长编码)                                    │
 * │   │  第一字节: [MSB][type:3][size:4]                         │
 * │   │  后续字节: [MSB][size:7]... (如果 MSB=1 则继续)           │
 * │   ├─ [OFS_DELTA: 负偏移 (变长编码)]                          │
 * │   ├─ [REF_DELTA: 基础对象 SHA (20 bytes)]                    │
 * │   └─ Zlib 压缩数据                                          │
 * │ Object 2: ...                                               │
 * │ ...                                                         │
 * ├─────────────────────────────────────────────────────────────┤
 * │ SHA-1 checksum (20 bytes)                                   │
 * └─────────────────────────────────────────────────────────────┘
 * 
 * 解析流程:
 * 1. 验证 "PACK" 头
 * 2. 读取对象数量
 * 3. 遍历每个对象:
 *    a. 解析类型和大小 (变长编码)
 *    b. 如果是 delta 对象，读取基础引用
 *    c. 解压 zlib 数据
 * 4. 第一遍: 处理非 delta 对象，计算 SHA
 * 5. 迭代解析 delta 对象，直到所有对象都被解析
 * 
 * @param packData 完整的 packfile 数据
 * @return map<SHA, PackObject> 所有解析后的对象
 */
std::map<std::string, PackObject> parsePackfile(const std::string& packData)
{
    std::map<std::string, PackObject> objects;      // SHA -> 对象
    std::map<size_t, std::string> offsetToSha;      // 偏移 -> SHA (用于 OFS_DELTA)

    // 验证 packfile 头
    if (packData.substr(0, 4) != "PACK")
        throw std::runtime_error("Invalid packfile");

    // 跳过版本号 (4 字节)，读取对象数量
    size_t pos = 8;
    uint32_t numObjects = (static_cast<unsigned char>(packData[pos]) << 24) |
                          (static_cast<unsigned char>(packData[pos+1]) << 16) |
                          (static_cast<unsigned char>(packData[pos+2]) << 8) |
                          static_cast<unsigned char>(packData[pos+3]);
    pos += 4;

    std::vector<PackObject> tempObjs;  // 临时存储所有对象

    // 第一阶段: 读取所有对象的原始数据
    for (uint32_t i = 0; i < numObjects; ++i)
    {
        size_t objOffset = pos;  // 记录对象起始位置
        PackObject obj;
        obj.offset = objOffset;

        // 读取类型和大小 (变长编码)
        // 第一字节: [MSB][type:3][size:4]
        unsigned char b = packData[pos++];
        obj.type = (b >> 4) & 0x07;  // 提取 type (bit 4-6)
        size_t size = b & 0x0f;       // 提取 size 低 4 位
        int shift = 4;
        
        // 如果 MSB=1，继续读取 size
        while (b & 0x80) {
            b = packData[pos++];
            size |= (b & 0x7f) << shift;  // 每次读取 7 位
            shift += 7;
        }

        // 处理 delta 对象的额外信息
        if (obj.type == OBJ_OFS_DELTA) {
            // OFS_DELTA: 读取负偏移 (特殊的变长编码)
            // 第一字节: [MSB][offset:7]
            // 后续字节: offset = ((offset + 1) << 7) | (byte & 0x7f)
            b = packData[pos++];
            size_t negOff = b & 0x7f;
            while (b & 0x80) {
                b = packData[pos++];
                negOff = ((negOff + 1) << 7) | (b & 0x7f);
            }
            obj.baseOffset = objOffset - negOff;  // 计算基础对象的绝对偏移
        } else if (obj.type == OBJ_REF_DELTA) {
            // REF_DELTA: 读取 20 字节的基础对象 SHA
            obj.baseSha = shaToHex(reinterpret_cast<const unsigned char*>(packData.data() + pos));
            pos += 20;
        }

        // 解压 zlib 数据
        auto [decompressed, consumed] = decompressZlibStream(packData, pos);
        obj.data = decompressed;
        pos += consumed;
        tempObjs.push_back(obj);
    }

    // 第二阶段: 处理非 delta 对象
    // 这些对象可以直接计算 SHA
    for (auto& obj : tempObjs) {
        if (obj.type != OBJ_OFS_DELTA && obj.type != OBJ_REF_DELTA) {
            // 构建完整对象: "type size\0content"
            std::string header = typeToString(obj.type) + " " + std::to_string(obj.data.size()) + '\0';
            obj.sha = computeSha1(header + obj.data);
            objects[obj.sha] = obj;
            offsetToSha[obj.offset] = obj.sha;
        }
    }

    // 第三阶段: 迭代解析 delta 对象
    // 需要迭代是因为 delta 可能依赖其他 delta 对象
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto& obj : tempObjs) {
            // 跳过已解析的对象
            if (!obj.sha.empty()) continue;

            // 查找基础对象的 SHA
            std::string baseSha;
            if (obj.type == OBJ_OFS_DELTA) {
                // OFS_DELTA: 通过偏移查找
                auto it = offsetToSha.find(obj.baseOffset);
                if (it != offsetToSha.end()) baseSha = it->second;
            } else if (obj.type == OBJ_REF_DELTA) {
                // REF_DELTA: 直接使用存储的 SHA
                baseSha = obj.baseSha;
            }
            if (baseSha.empty()) continue;

            // 查找基础对象
            auto baseIt = objects.find(baseSha);
            if (baseIt == objects.end()) continue;  // 基础对象尚未解析

            // 应用 delta 重建完整对象
            obj.data = applyDelta(baseIt->second.data, obj.data);
            obj.type = baseIt->second.type;  // 继承基础对象的类型
            
            // 计算 SHA
            std::string header = typeToString(obj.type) + " " + std::to_string(obj.data.size()) + '\0';
            obj.sha = computeSha1(header + obj.data);
            objects[obj.sha] = obj;
            offsetToSha[obj.offset] = obj.sha;
            progress = true;  // 有进展，继续迭代
        }
    }
    return objects;
}

/**
 * 从 .git/objects/ 读取并解析一个 Git 对象
 * 
 * Git 对象存储格式:
 * - 路径: .git/objects/XX/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *   (XX 是 SHA 前两个字符，后面是剩余 38 个字符)
 * - 内容: zlib 压缩的 "type size\0content"
 * 
 * @param sha 对象的 40 字符十六进制 SHA
 * @return pair<类型字符串, 对象内容>
 * @throws std::runtime_error 如果对象不存在
 * 
 * 示例:
 * 读取 sha="abc123..." 
 * -> 打开 .git/objects/ab/c123...
 * -> 解压得到 "blob 5\0hello"
 * -> 返回 {"blob", "hello"}
 */
std::pair<std::string, std::string> readObject(const std::string& sha)
{
    std::string path = g_gitDir + "/objects/" + sha.substr(0,2) + "/" + sha.substr(2);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Object not found: " + sha);

    std::vector<char> compressed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string decompressed = decompressZlib(compressed);

    // 解析头部: "type size\0content"
    size_t nullPos = decompressed.find('\0');
    std::string header = decompressed.substr(0, nullPos);
    std::string content = decompressed.substr(nullPos + 1);
    std::string type = header.substr(0, header.find(' '));
    return {type, content};
}

/**
 * 递归检出 tree 对象到文件系统
 * 
 * Tree 对象格式 (解压后的内容部分):
 * ┌────────────────────────────────────────┐
 * │ "<mode> <name>\0<20-byte-sha>"         │
 * │ "<mode> <name>\0<20-byte-sha>"         │
 * │ ...                                    │
 * └────────────────────────────────────────┘
 * 
 * mode 类型:
 * - "40000": 目录 (tree)
 * - "100644": 普通文件 (blob)
 * - "100755": 可执行文件 (blob)
 * - "120000": 符号链接
 * 
 * @param treeSha tree 对象的 SHA
 * @param dir 目标目录路径
 * 
 * 执行流程:
 * 1. 读取 tree 对象
 * 2. 遍历每个条目:
 *    - 解析 mode, name, sha
 *    - 如果是目录 (40000): 创建目录，递归调用 checkoutTree
 *    - 如果是文件: 读取 blob 对象，写入文件
 */
void checkoutTree(const std::string& treeSha, const std::filesystem::path& dir)
{
    // 读取并验证 tree 对象
    auto [type, content] = readObject(treeSha);
    if (type != "tree") throw std::runtime_error("Expected tree");

    size_t pos = 0;
    while (pos < content.size())
    {
        // 解析 mode (到空格为止)
        size_t spacePos = content.find(' ', pos);
        std::string mode = content.substr(pos, spacePos - pos);
        pos = spacePos + 1;

        // 解析 name (到 null 字节为止)
        size_t nullPos = content.find('\0', pos);
        std::string name = content.substr(pos, nullPos - pos);
        pos = nullPos + 1;

        // 读取 20 字节原始 SHA 并转换为十六进制
        std::string sha = shaToHex(reinterpret_cast<const unsigned char*>(content.data() + pos));
        pos += 20;

        std::filesystem::path entryPath = dir / name;

        if (mode == "40000") {
            // 目录: 创建并递归处理
            std::filesystem::create_directories(entryPath);
            checkoutTree(sha, entryPath);
        } else {
            // 文件: 读取 blob 内容并写入
            auto [objType, fileContent] = readObject(sha);
            std::ofstream out(entryPath, std::ios::binary);
            out.write(fileContent.data(), fileContent.size());
        }
    }
}

/**
 * 克隆远程 Git 仓库
 * 
 * 完整执行流程:
 * ═══════════════════════════════════════════════════════════════════════════
 * 
 * 步骤 1: 初始化本地目录结构
 * ───────────────────────────────────────────────────────────────────────────
 *   创建: targetDir/
 *         targetDir/.git/
 *         targetDir/.git/objects/
 *         targetDir/.git/refs/heads/
 * 
 * 步骤 2: 发现远程引用 (Reference Discovery)
 * ───────────────────────────────────────────────────────────────────────────
 *   请求: GET {repoUrl}/info/refs?service=git-upload-pack
 *   
 *   响应示例 (pkt-line 格式):
 *   001e# service=git-upload-pack
 *   0000
 *   00a8abc123... HEAD\0multi_ack thin-pack side-band...
 *   003fabc123... refs/heads/master
 *   0000
 *   
 *   解析: 提取 HEAD 的 SHA 和默认分支 (master/main)
 * 
 * 步骤 3: 请求 Packfile
 * ───────────────────────────────────────────────────────────────────────────
 *   请求: POST {repoUrl}/git-upload-pack
 *   Content-Type: application/x-git-upload-pack-request
 *   
 *   请求体:
 *   0032want <head_sha>
 *   0000
 *   0009done
 *   
 *   响应: NAK + PACK 数据
 * 
 * 步骤 4: 解析 Packfile
 * ───────────────────────────────────────────────────────────────────────────
 *   调用 parsePackfile() 解析所有对象
 *   处理普通对象和 delta 对象
 * 
 * 步骤 5: 写入对象到 .git/objects/
 * ───────────────────────────────────────────────────────────────────────────
 *   对每个对象:
 *   - 构建完整对象: "type size\0content"
 *   - zlib 压缩
 *   - 写入 .git/objects/XX/XXXX...
 * 
 * 步骤 6: 设置 HEAD 和 refs
 * ───────────────────────────────────────────────────────────────────────────
 *   写入 .git/HEAD: "ref: refs/heads/master\n"
 *   写入 .git/refs/heads/master: "<sha>\n"
 * 
 * 步骤 7: 检出工作目录
 * ───────────────────────────────────────────────────────────────────────────
 *   读取 HEAD commit -> 获取 tree SHA -> 递归检出文件
 * 
 * @param repoUrl 远程仓库 URL (如 https://github.com/user/repo)
 * @param targetDir 本地目标目录
 */
void cloneRepository(const std::string& repoUrl, const std::string& targetDir)
{
    // ═══════════════════════════════════════════════════════════════════════
    // 步骤 1: 初始化本地目录结构
    // ═══════════════════════════════════════════════════════════════════════
    std::filesystem::create_directories(targetDir);
    g_gitDir = targetDir + "/.git";
    std::filesystem::create_directories(g_gitDir + "/objects");
    std::filesystem::create_directories(g_gitDir + "/refs/heads");

    // ═══════════════════════════════════════════════════════════════════════
    // 步骤 2: 发现远程引用 (Reference Discovery)
    // ═══════════════════════════════════════════════════════════════════════
    // 请求远程仓库的引用列表
    std::string refsUrl = repoUrl + "/info/refs?service=git-upload-pack";
    std::string refsResp = httpGet(refsUrl);
    
    // 解析 pkt-line 格式的响应
    auto lines = parsePktLines(refsResp);

    // 提取 HEAD SHA 和默认分支
    std::string headSha, headRef;
    for (const auto& line : lines) {
        // 跳过服务声明行
        if (line.find("# service=") != std::string::npos) continue;
        
        // 解析格式: "<sha> <ref>[\0<capabilities>]"
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        
        std::string sha = line.substr(0, sp);
        if (sha.length() != 40) continue;  // 验证 SHA 长度
        
        // 提取 ref 名称 (可能后面有 \0 分隔的 capabilities)
        std::string refPart = line.substr(sp + 1);
        size_t np = refPart.find('\0');
        std::string ref = (np != std::string::npos) ? refPart.substr(0, np) : refPart;

        // 记录 HEAD 和默认分支
        if (ref == "HEAD") headSha = sha;
        else if (ref == "refs/heads/master" || ref == "refs/heads/main") {
            if (headSha.empty()) headSha = sha;
            headRef = ref;
        }
    }
    if (headSha.empty()) throw std::runtime_error("No HEAD found");

    // ═══════════════════════════════════════════════════════════════════════
    // 步骤 3: 请求 Packfile
    // ═══════════════════════════════════════════════════════════════════════
    // 构建 want 请求: 告诉服务器我们需要哪些对象
    // 格式: want <sha>\n + flush + done\n
    std::string req = createPktLine("want " + headSha + "\n") + "0000" + createPktLine("done\n");
    
    // 发送 POST 请求获取 packfile
    std::string packResp = httpPost(repoUrl + "/git-upload-pack", req, "application/x-git-upload-pack-request");

    // 在响应中找到 PACK 数据的起始位置
    // 响应格式: NAK\n + PACK...
    size_t packPos = packResp.find("PACK");
    if (packPos == std::string::npos) throw std::runtime_error("No packfile");
    std::string packData = packResp.substr(packPos);

    // ═══════════════════════════════════════════════════════════════════════
    // 步骤 4 & 5: 解析 Packfile 并写入对象
    // ═══════════════════════════════════════════════════════════════════════
    auto objects = parsePackfile(packData);
    
    // 将所有对象写入 .git/objects/
    for (const auto& [sha, obj] : objects) {
        // 构建完整对象数据: "type size\0content"
        std::string header = typeToString(obj.type) + " " + std::to_string(obj.data.size()) + '\0';
        writeObjectWithSha(header + obj.data, sha);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 步骤 6: 设置 HEAD 和 refs
    // ═══════════════════════════════════════════════════════════════════════
    std::ofstream headFile(g_gitDir + "/HEAD");
    if (headRef.empty()) {
        // 没有分支引用，直接写入 SHA (detached HEAD)
        headFile << headSha << "\n";
    } else {
        // 写入符号引用
        headFile << "ref: " << headRef << "\n";
        
        // 同时写入分支文件
        std::string refPath = g_gitDir + "/" + headRef;
        std::filesystem::create_directories(std::filesystem::path(refPath).parent_path());
        std::ofstream(refPath) << headSha << "\n";
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 步骤 7: 检出工作目录
    // ═══════════════════════════════════════════════════════════════════════
    // 读取 HEAD 指向的 commit 对象
    auto [commitType, commitContent] = readObject(headSha);
    
    // 从 commit 内容中提取 tree SHA
    // commit 格式:
    // tree <sha>
    // parent <sha>
    // author ...
    // committer ...
    // 
    // <message>
    std::istringstream iss(commitContent);
    std::string line, treeSha;
    while (std::getline(iss, line)) {
        if (line.substr(0, 5) == "tree ") { 
            treeSha = line.substr(5); 
            break; 
        }
    }
    if (treeSha.empty()) throw std::runtime_error("No tree in commit");
    
    // 递归检出 tree 到工作目录
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
