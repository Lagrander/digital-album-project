#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 播放指定 URL 的 MP3 文件
void stream_player_play_url(const char *url);

// 播放指定 URL 的 MP3 文件，支持循环（EOF 后自动重连）
void stream_player_play_url_with_loop(const char *url, bool loop);

// 停止当前流媒体播放
void stream_player_stop(void);

// 获取当前是否正在播放
bool stream_player_is_playing(void);

#ifdef __cplusplus
}
#endif
