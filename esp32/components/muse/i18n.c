/*
 * 界面中文字符串表。由 tools/gen-i18n.py 从 docs/i18n-plan.md 生成，勿手改。
 * 只做中文：tr() 查表，未命中回退英文原文（便于新加文案时不至于空白）。
 */
#include "i18n.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *en;
    const char *zh;
} i18n_entry_t;

static const i18n_entry_t s_zh[] = {
    {"Connecting...", "连接中…"},
    {"MUSE DIDN'T TAKE IT", "Muse 未接收"},
    {"LOST CONNECTION TO MUSE", "与 Muse 连接断开"},
    {"MUSE REPLY ACCESS DENIED (403)", "Muse 拒绝访问 (403)"},
    {"MUSE REPLY AUTH REQUIRED (401)", "Muse 回复需鉴权 (401)"},
    {"NO REPLY FROM MUSE", "Muse 无回复"},
    {"REPLY TOO LONG", "回复过长"},
    {"REPLY BUFFER LIMIT - TRY AGAIN", "回复超限，请重试"},
    {"OUT OF MEMORY", "内存不足"},
    {"CAN'T KEEP UP", "处理不过来"},
    {"CAN'T SUBSCRIBE TO MUSE", "无法订阅 Muse"},
    {"GOODBYE!", "再见！"},
    {"COULDN'T POWER OFF", "关机失败"},
    {"RESETTING SETUP", "正在重置配对"},
    {"RELEASE TO RESET SETUP", "松手即重置配对"},
    {"RELEASE TO POWER OFF", "松手即关机"},
    {"HOLD 2s OFF / 5s RESET", "长按2秒关机 / 5秒重置"},
    {"HOLD TO POWER OFF", "长按关机"},
    {"LINE TOO LONG", "输入过长"},
    {"TOO LONG", "过长"},
    {"THIS BOARD CAN'T CHAT OVER SERIAL", "此板不支持串口聊天"},
    {"Starting...", "启动中…"},
    {"POWER OFF", "关机"},
    {"RESET PAIRING", "重置配对"},
    {"RESETTING...", "重置中…"},
    {"AUTO-SLEEP", "自动休眠"},
    {"Saved networks", "已保存网络"},
    {"WI-FI", "Wi-Fi"},
    {"TAP TO TAKE PHOTO", "轻触拍照"},
    {"Pairing code", "配对码"},
    {"WI-FI OFF", "Wi-Fi 已关闭"},
    {"SET UP WI-FI", "请连接 Wi-Fi"},
    {"NO WI-FI", "无 Wi-Fi"},
    {"SET UP MUSE FIRST", "请先配对 Muse"},
    {"USB POWER", "USB 供电"},
    {"WAKING UP", "唤醒中"},
    {"SPEAKER ON", "扬声器开"},
    {"SPEAKER OFF", "扬声器关"},
    {"HOLD TO MUTE", "长按静音"},
    {"HOLD TO UNMUTE", "长按取消静音"},
    {"LISTENING...", "聆听中…"},
    {"RECORDING...", "录音中…"},
    {"SENDING VOICE NOTE", "发送语音…"},
    {"NOTE SENT - WAITING FOR MUSE", "已发送，等待 Muse"},
    {"CAN'T REACH MUSE", "无法连接 Muse"},
    {"COULDN'T SAVE THE NOTE", "语音保存失败"},
    {"SAVED, SENDS WHEN ONLINE", "已保存，联网后发送"},
    {"SAVED, WILL TRY AGAIN", "已保存，稍后重试"},
    {"SENDING SAVED NOTE", "发送已存语音"},
    {"SAVED NOTE SENT", "已存语音已发送"},
    {"COULDN'T SEND A SAVED NOTE", "已存语音发送失败"},
    {"SAVED NOTE: WILL TRY AGAIN", "已存语音：稍后重试"},
    {"NOTES STILL WAITING TO SEND", "还有语音待发送"},
    {"HOLD LONGER TO TALK", "按住时间再长些"},
    {"AUDIO INIT FAILED", "音频初始化失败"},
    {"Volume", "音量"},
    {"Joining...", "连接中…"},
    {"No saved network nearby", "附近无已保存网络"},
    {"Can't join; retrying", "连接失败，重试中"},
    {"Waiting for Wi-Fi", "等待 Wi-Fi"},
    {"Not set up", "未配对"},
    {"Offline", "离线"},
    {"Saved", "已保存"},
    {"Connecting", "连接中"},
    {"Connected", "已连接"},
    {"Can't connect", "无法连接"},
    {"[Voice note]", "[语音]"},
    {"Starting", "启动中"},
    {"Ready to pair", "可配对"},
    {"App connected", "App 已连接"},
    {"Confirm pairing", "确认配对"},
    {"Online", "在线"},
    {"Error", "错误"},
    {"Toggle", "切换"},
    {"Open", "打开"},
    {"Select", "选择"},
    {"Close", "关闭"},
    {"30 s", "30 秒"},
    {"1 min", "1 分钟"},
    {"2 min", "2 分钟"},
    {"5 min", "5 分钟"},
    {"10 min", "10 分钟"},
    {"Never", "从不"},
    {"Custom", "自定义"},
    {"Failed", "失败"},
    {"Not found", "未找到"},
    {"Joining", "连接中"},
    {"Not set", "未设置"},
    {"Off", "关"},
    {"On batt", "用电池时"},
    {"Last run", "上次运行"},
    {"Back", "返回"},
    {"Esc", "返回"},
    {"Cancel", "取消"},
    {"Reset", "重置"},
    {"Speaker", "扬声器"},
    {"Brightness", "亮度"},
    {"Mic gain", "麦克风增益"},
    {"Auto-sleep", "自动休眠"},
    {"Phone setup", "手机配对"},
    {"Wi-Fi", "Wi-Fi"},
    {"Status", "状态"},
    {"Battery", "电量"},
    {"Reset pairing", "重置配对"},
    {"Screen off", "关闭屏幕"},
    {"Power off", "关机"},
    {"Close menu", "关闭菜单"},
    {"Change", "调整"},
    {"30 seconds", "30 秒"},
    {"1 minute", "1 分钟"},
    {"2 minutes", "2 分钟"},
    {"5 minutes", "5 分钟"},
    {"10 minutes", "10 分钟"},
    {"Mic level", "麦克风电平"},
    {"Used", "已用"},
    {"A full charge", "满电续航"},
    {"Chip asleep", "芯片休眠"},
    {"Wakes", "唤醒次数"},
    {"CPU busy", "CPU 占用"},
    {"No battery", "无电池"},
    {"None", "无"},
    {"Muse", "Muse"},
    {"Bluetooth", "蓝牙"},
    {"Sound", "声音"},
    {"Sleep", "休眠"},
    {"Muted", "已静音"},
    {"Hide", "隐藏"},
    {"Show", "显示"},
    {"Other network", "其他网络"},
    {"Hidden", "已隐藏"},
    {"No networks found", "未找到网络"},
    {"Other network...", "其他网络…"},
    {"MAC address", "MAC 地址"},
    {"Wi-Fi is off", "Wi-Fi 已关闭"},
    {"Scanning...", "扫描中…"},
    {"Muse server", "Muse 服务器"},
    {"VM ID", "VM ID"},
    {"Device token", "设备令牌"},
    {"Server", "服务器"},
    {"Test connection", "测试连接"},
    {"Forget paired phones", "忘记已配对手机"},
    {"Pair in the Muse app", "在 Muse App 里配对"},
    {"Connects when you talk", "说话时自动连接"},
    {"Through Home Link, text replies", "经 Home Link 回文字"},
    {"Keypad  Cancel  Accept", "键盘  取消  确定"},
    {"Forget Wi-Fi and the Muse app pairing, then restart?", "将忘记 Wi-Fi 和 Muse 配对，然后重启？"},
    {"MENU  ^v Move  <> Change", "菜单  ^v 移动  <> 调整"},
    {"Turn the screen off after Muse has been idle for:", "Muse 空闲多久后关闭屏幕："},
    {"Tap the screen or press either button to wake.", "轻触屏幕或按任意键唤醒"},
    {"Unplug USB to start measuring.", "拔掉 USB 开始测量"},
    {"Power Muse off completely?", "彻底关闭 Muse？"},
    {"Tap to forget", "轻触以忘记"},
    {"No saved networks. Scan and pick one.", "没有已保存的网络，请扫描选择"},
    {"Empty for the default", "留空使用默认"},
    {"Optional", "可选"},
    {"Tap again to reset", "再轻触一次以重置"},
    {"Enter on phone", "在手机上输入"},
    {"Enter it on your phone", "在手机上输入"},
    {"bottom right button", "右下按键"},
    {"Press button", "按按键"},
    {"Press", "按"},
    {"Pair with Muse app", "用 Muse App 配对"},
    {"Muse app", "Muse App"},
    {"Scan for networks", "扫描网络"},
    {"Sleep now", "立即休眠"},
    {"Start over", "重新开始"},
    {"BATTERY", "电池"},
    {"STATUS", "状态"},
    {"MENU", "菜单"},
    {"Resetting...", "重置中…"},
    {"Down", "向下"},
    {"Esc Back", "返回"},
    {"READY", "就绪"},
    {"LISTENING", "聆听中"},
    {"THINKING", "思考中"},
    {"SPEAKING", "说话中"},
    {"ERROR", "错误"},
    {"GOODBYE", "再见"},
    {"Muse remembers up to 8 networks and joins the strongest one in range. Tap a saved one twice to forget it.", "Muse 最多记住 8 个网络，自动连上信号最强的那个；轻触已保存网络两次即可忘记。"},
    {"When on, Muse is visible to phones nearby. Open tools/ble_setup.html in Chrome, connect, and enter the code Muse shows to pair.", "开启后附近的手机可发现 Muse：用 Chrome 打开 tools/ble_setup.html，连接后输入 Muse 显示的配对码完成配对。"},
    {"SETTINGS", "设置"},
    {"SOUND", "声音"},
    {"POWER", "电源"},
    {"BLUETOOTH", "蓝牙"},
    {"MUSE", "Muse 配对"},
    {"On", "开"},
    {"Vol", "音量"},
    {"Scan to set up", "扫码配网"},
    {"Hotspot", "热点"},
    {"Scan to join. Tap anywhere to close this", "扫码加入热点；轻触屏幕关闭本页"},
    {"Password", "密码"},
    {"Set up Wi-Fi on your phone", "用手机配网"},
    {"Same Wi-Fi as the board: scan to open", "手机连同一个 Wi-Fi，扫码打开"},
    {"Back to home", "返回首页"},
};

const char *tr(const char *en)
{
    if (en == NULL) {
        return "";
    }
    for (size_t i = 0; i < sizeof(s_zh) / sizeof(s_zh[0]); ++i) {
        if (strcmp(s_zh[i].en, en) == 0) {
            return s_zh[i].zh;
        }
    }
    return en;
}

/* 点阵中文字体没有 LVGL 的 LV_SYMBOL_*（私有区，靠字体 fallback 渲染），
 * 而 "ICON" "text" 是编译期拼接、包不了 tr()，所以图标+文案用运行时拼接。
 * 返回静态缓冲；调用方紧接着就复制（lv_label_set_text 会拷贝），UI 单任务安全。 */
static const char *icon_text(const char *icon, const char *en, bool icon_first)
{
    static char buf[96];
    const char *text = tr(en);

    if (icon == NULL) {
        icon = "";
    }
    if (icon_first) {
        (void)snprintf(buf, sizeof(buf), "%s  %s", icon, text);
    } else {
        (void)snprintf(buf, sizeof(buf), "%s %s", text, icon);
    }
    return buf;
}

const char *tr_icon(const char *icon, const char *en)
{
    return icon_text(icon, en, true);
}

const char *tr_icon_tail(const char *en, const char *icon)
{
    return icon_text(icon, en, false);
}
