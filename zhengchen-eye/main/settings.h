#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <nvs_flash.h>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    void EraseKey(const std::string& key);
    void EraseAll();
    // 从NVS读取模式标志位
    int32_t read_mode_flag();
    // 写入模式标志位到NVS
    void write_mode_flag(int32_t mode);

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
public:
    int32_t current_mode = 0;     // 当前模式：0=文字模式，1=表情模式
};

#endif
