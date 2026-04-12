#include "wiHelper.h"
#include "wiArchive.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace wi::helper
{

std::string GetFileNameFromPath(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string GetDirectoryFromPath(const std::string& path)
{
    const std::filesystem::path p(path);
    const std::filesystem::path parent = p.parent_path();
    if (parent.empty())
    {
        return "";
    }
    std::string result = parent.string();
    if (!result.empty() && result.back() != '/' && result.back() != '\\')
    {
        result.push_back('/');
    }
    return result;
}

std::string ReplaceExtension(const std::string& filename, const std::string& extension)
{
    std::filesystem::path p(filename);
    p.replace_extension(extension);
    return p.string();
}

void MakePathRelative(const std::string& rootdir, std::string& path)
{
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, rootdir, ec);
    if (!ec)
    {
        path = rel.string();
    }
}

void MakePathAbsolute(std::string& path)
{
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (!ec)
    {
        path = abs.string();
    }
}

void DirectoryCreate(const std::string& path)
{
    if (path.empty())
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    (void)ec;
}

bool FileRead(const std::string& fileName, wi::vector<uint8_t>& data, size_t max_read, size_t offset)
{
    std::ifstream file(fileName, std::ios::binary);
    if (!file)
    {
        return false;
    }

    file.seekg(0, std::ios::end);
    const size_t fileSize = static_cast<size_t>(file.tellg());
    if (offset >= fileSize)
    {
        data.clear();
        return true;
    }
    const size_t available = fileSize - offset;
    const size_t toRead = std::min(available, max_read);
    data.resize(toRead);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(toRead));
    return file.good() || file.eof();
}

#if WI_VECTOR_TYPE
bool FileRead(const std::string& fileName, std::vector<uint8_t>& data, size_t max_read, size_t offset)
{
    wi::vector<uint8_t> tmp;
    if (!FileRead(fileName, tmp, max_read, offset))
    {
        return false;
    }
    data.assign(tmp.begin(), tmp.end());
    return true;
}
#endif

bool FileWrite(const std::string& fileName, const uint8_t* data, size_t size)
{
    std::ofstream file(fileName, std::ios::binary);
    if (!file)
    {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

bool FileExists(const std::string& fileName)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(fileName), ec);
}

uint64_t FileTimestamp(const std::string& fileName)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(std::filesystem::path(fileName), ec);
    if (ec)
    {
        return 0;
    }
    const auto count = time.time_since_epoch().count();
    return static_cast<uint64_t>(count > 0 ? count : 0);
}

void StringConvert(const std::string& from, std::wstring& to)
{
    to.assign(from.begin(), from.end());
}

void StringConvert(const std::wstring& from, std::string& to)
{
    to.assign(from.begin(), from.end());
}

int StringConvert(const char* from, wchar_t* to, int dest_size_in_characters)
{
    if (to == nullptr || dest_size_in_characters <= 0)
    {
        return 0;
    }
    const std::string src = from != nullptr ? from : "";
    const int count = std::min(static_cast<int>(src.size()), dest_size_in_characters - 1);
    for (int i = 0; i < count; ++i)
    {
        to[i] = static_cast<wchar_t>(src[i]);
    }
    to[count] = L'\0';
    return count;
}

int StringConvert(const wchar_t* from, char* to, int dest_size_in_characters)
{
    if (to == nullptr || dest_size_in_characters <= 0)
    {
        return 0;
    }
    const std::wstring src = from != nullptr ? from : L"";
    const int count = std::min(static_cast<int>(src.size()), dest_size_in_characters - 1);
    for (int i = 0; i < count; ++i)
    {
        to[i] = static_cast<char>(src[i]);
    }
    to[count] = '\0';
    return count;
}

} // namespace wi::helper

namespace wi
{

Archive::Archive()
{
    CreateEmpty();
}

Archive::Archive(const std::string& fileName, bool readMode)
{
    this->fileName = fileName;
    directory = wi::helper::GetDirectoryFromPath(fileName);

    if (readMode)
    {
        this->readMode = true;
        if (!wi::helper::FileRead(fileName, DATA))
        {
            DATA.clear();
            data_ptr = nullptr;
            data_ptr_size = 0;
            pos = 0;
            return;
        }
        data_ptr = DATA.data();
        data_ptr_size = DATA.size();
        if (data_ptr_size >= sizeof(Header))
        {
            std::memcpy(&header, data_ptr, sizeof(Header));
            pos = sizeof(Header);
        }
        else
        {
            pos = 0;
        }
        return;
    }

    CreateEmpty();
    this->fileName = fileName;
    directory = wi::helper::GetDirectoryFromPath(fileName);
}

Archive::Archive(const uint8_t* data, size_t size)
{
    readMode = true;
    if (data != nullptr && size > 0)
    {
        DATA.assign(data, data + size);
        data_ptr = DATA.data();
        data_ptr_size = DATA.size();
        if (data_ptr_size >= sizeof(Header))
        {
            std::memcpy(&header, data_ptr, sizeof(Header));
            pos = sizeof(Header);
        }
    }
}

void Archive::WriteCompressedData(wi::vector<uint8_t>& final_data) const
{
    final_data = DATA;
}

void Archive::CreateEmpty()
{
    header = {};
    readMode = false;
    pos = sizeof(Header);
    DATA.resize(sizeof(Header) * 2);
    std::memcpy(DATA.data(), &header, sizeof(Header));
    data_ptr = DATA.data();
    data_ptr_size = DATA.size();
    data_already_decompressed = true;
}

void Archive::SetReadModeAndResetPos(bool isReadMode)
{
    readMode = isReadMode;
    if (DATA.empty())
    {
        CreateEmpty();
    }
    pos = sizeof(Header);
}

void Archive::Close()
{
    if (!readMode && !fileName.empty() && !DATA.empty())
    {
        std::memcpy(DATA.data(), &header, sizeof(Header));
        const size_t writeSize = std::min(pos, DATA.size());
        wi::helper::DirectoryCreate(wi::helper::GetDirectoryFromPath(fileName));
        wi::helper::FileWrite(fileName, DATA.data(), writeSize);
    }
    DATA.clear();
    data_ptr = nullptr;
    data_ptr_size = 0;
    pos = 0;
}

const std::string& Archive::GetSourceDirectory() const
{
    return directory;
}

const std::string& Archive::GetSourceFileName() const
{
    return fileName;
}

} // namespace wi
