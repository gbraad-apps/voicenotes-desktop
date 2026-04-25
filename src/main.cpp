/**
 * Voicenotes — Desktop voice recorder + Whisper transcriber
 * Uses same SDL3 stack as Junglizer (rfx/common/sdl_audio_init.h, regroove_ui.h)
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl2.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include "regroove_ui.h"      // setupStyle, renderBanner, Colors
#include "sdl_audio_init.h"   // sdl_audio_init — same as junglizer
#include "whisper.h"
#include "tinyfiledialogs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ── Constants ─────────────────────────────────────────────────────────────────
static const int SAMPLE_RATE = 16000; // whisper expects 16 kHz

static const char* LANGUAGES[] = {
    "auto","en","de","nl","fr","es","it","pt","ru","zh","ja","ko","ar"
};
static const char* LANG_NAMES[] = {
    "Auto-detect","English","German","Dutch","French","Spanish",
    "Italian","Portuguese","Russian","Chinese","Japanese","Korean","Arabic"
};
static const int LANG_COUNT = 13;

// ── App state ─────────────────────────────────────────────────────────────────
enum class RecordState { Idle, Recording, Paused, Transcribing, Done };

struct App {
    // SDL / GL
    SDL_Window*      window     = nullptr;
    SDL_GLContext    gl_context = nullptr;

    // Capture (recording from mic via SDL3 stream callback)
    SDL_AudioStream* cap_stream = nullptr;
    SDL_AudioDeviceID cap_dev   = 0;

    // Playback
    SDL_AudioStream* play_stream  = nullptr;
    SDL_AudioDeviceID play_dev    = 0;
    bool             playing      = false;
    std::string      playback_file;

    // Recording data
    RecordState      state       = RecordState::Idle;
    std::vector<float> samples;
    std::mutex       mtx;
    float            level       = 0.f;
    std::string      take_name;
    int              take_counter = 1;

    // Whisper
    whisper_context* wctx        = nullptr;
    std::string      model_path;
    std::atomic<int> progress{0};

    // Transcript — char buffer so ImGui InputTextMultiline can make it selectable/copyable
    static const int TBUF = 1 << 17; // 128 KB
    char             tbuf[TBUF]{};

    // Files
    std::string      folder;
    std::vector<std::string> files;
    int              sel_file   = -1;

    // UI
    bool             show_settings = false;
    int              lang_idx    = 0;
    std::string      status      = "Ready";

    // Model download
    std::atomic<bool>   downloading{false};
    std::atomic<bool>   download_cancel{false};
    std::atomic<float>  download_progress{0.f};
    std::atomic<size_t> download_bytes{0};
    std::string         download_dest;
    std::string         download_last_url;   // for retry
    std::string         download_error;      // last error message
};

static App      g_app;
static bool     g_running = true;

// Audio device list (populated at startup)
static std::vector<std::string> g_input_devices;
static int                      g_input_device_idx = 0; // 0 = default

static std::string g_config_path;

static void save_config() {
    if (g_config_path.empty()) return;
    std::ofstream f(g_config_path);
    f << "folder="      << g_app.folder      << "\n";
    f << "model="       << g_app.model_path  << "\n";
    f << "lang="        << g_app.lang_idx    << "\n";
    f << "input_dev="   << g_input_device_idx << "\n";
}

static void load_config() {
    if (g_config_path.empty()) return;
    std::ifstream f(g_config_path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "folder"    && !val.empty() && fs::exists(val)) g_app.folder = val;
        if (key == "model"     && !val.empty() && fs::exists(val)) g_app.model_path = val;
        if (key == "lang"      && !val.empty()) g_app.lang_idx = std::stoi(val);
        if (key == "input_dev" && !val.empty()) g_input_device_idx = std::stoi(val);
    }
}

// Audio device list (populated at startup)
// Forward declarations
static void capture_callback(void*, SDL_AudioStream*, int, int);
static void load_model(const std::string& path);

// ── In-app model download via libcurl ─────────────────────────────────────────
struct DlState {
    FILE*               f;
    std::atomic<float>* progress;
    std::atomic<size_t>* bytes;
};

static int dl_progress_cb(void* ud, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* ds = (DlState*)ud;
    if (g_app.download_cancel.load()) return 1; // abort
    if (total > 0) *ds->progress = (float)now / (float)total;
    else           *ds->progress = -1.f; // indeterminate
    return 0;
}
static size_t dl_write_cb(void* ptr, size_t sz, size_t n, void* ud) {
    auto* ds = (DlState*)ud;
    size_t written = fwrite(ptr, sz, n, ds->f);
    *ds->bytes += written;
    return written;
}

static void download_model(const std::string& url, const std::string& dest) {
    if (g_app.downloading) return;
    g_app.downloading = true;
    g_app.download_cancel = false;
    g_app.download_progress = -1.f;
    g_app.download_bytes = 0;
    g_app.download_dest = dest;
    g_app.download_last_url = url;
    g_app.download_error.clear();

    std::thread([url, dest]() {
        std::string tmp = dest + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) { g_app.status = "Cannot write: " + tmp; g_app.downloading = false; return; }

        DlState ds{ f, &g_app.download_progress, &g_app.download_bytes };
        CURL* curl = curl_easy_init();

        // HuggingFace requires browser-like headers and proper redirect handling
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Accept: application/octet-stream");
        hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache");

        curl_easy_setopt(curl, CURLOPT_URL,              url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       hdrs);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS,        10L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    dl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &ds);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &ds);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Voicenotes/1.0");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,   0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,  1L);   // abort if < 1 byte/s
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,   60L);  // for 60 seconds

        // Show connecting status before perform blocks
        g_app.status = "Connecting to HuggingFace...";

        CURLcode res = curl_easy_perform(curl);

        // Check HTTP response code even if CURLE_OK
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        fclose(f);

        if (g_app.download_cancel.load()) {
            std::remove(tmp.c_str());
            g_app.status = "Download cancelled";
        } else if (res == CURLE_OK && http_code == 200) {
            std::rename(tmp.c_str(), dest.c_str());
            g_app.status = "Downloaded: " + fs::path(dest).filename().string();
            load_model(dest);
        } else {
            std::remove(tmp.c_str());
            if (res != CURLE_OK)
                g_app.download_error = std::string(curl_easy_strerror(res));
            else
                g_app.download_error = "HTTP " + std::to_string(http_code);
            g_app.status = "Download failed: " + g_app.download_error;
        }
        g_app.download_cancel = false;
        g_app.downloading = false;
        g_app.download_progress = 0.f;
        g_app.download_bytes = 0;
    }).detach();
}

// ── Enumerate input devices ───────────────────────────────────────────────────
static void enumerate_inputs() {
    g_input_devices.clear();
    g_input_devices.push_back("Default");
    int count = 0;
    SDL_AudioDeviceID* devs = SDL_GetAudioRecordingDevices(&count);
    if (devs) {
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(devs[i]);
            if (name) g_input_devices.push_back(name);
        }
        SDL_free(devs);
    }
}

static void open_capture_device(int idx) {
    if (g_app.cap_stream) { SDL_DestroyAudioStream(g_app.cap_stream); g_app.cap_stream = nullptr; }

    SDL_AudioDeviceID dev_id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (idx > 0) {
        int count = 0;
        SDL_AudioDeviceID* devs = SDL_GetAudioRecordingDevices(&count);
        if (devs && idx-1 < count) dev_id = devs[idx-1];
        if (devs) SDL_free(devs);
    }

    SDL_AudioSpec cap_spec{}; cap_spec.format = SDL_AUDIO_F32; cap_spec.channels = 1; cap_spec.freq = SAMPLE_RATE;
    g_app.cap_stream = SDL_OpenAudioDeviceStream(dev_id, &cap_spec, capture_callback, &g_app);
    if (g_app.cap_stream) {
        g_app.cap_dev = SDL_GetAudioStreamDevice(g_app.cap_stream);
        SDL_PauseAudioDevice(g_app.cap_dev);
        g_input_device_idx = idx;
    } else {
        g_app.status = std::string("Cannot open mic: ") + SDL_GetError();
    }
}

// ── Capture callback (SDL3 recording) ─────────────────────────────────────────
// Called by SDL when new captured audio is available in the stream
static void capture_callback(void* userdata, SDL_AudioStream* stream,
                              int additional_amount, int /*total*/) {
    App* app = (App*)userdata;
    if (app->state != RecordState::Recording || additional_amount <= 0) return;

    int frames = additional_amount / sizeof(float);
    std::vector<float> buf(frames);
    if (SDL_GetAudioStreamData(stream, buf.data(), additional_amount) <= 0) return;

    std::lock_guard<std::mutex> lk(app->mtx);
    app->samples.insert(app->samples.end(), buf.begin(), buf.end());

    float rms = 0;
    for (float s : buf) rms += s * s;
    app->level = std::sqrt(rms / frames);
}

// ── WAV save ──────────────────────────────────────────────────────────────────
static bool save_wav(const std::string& path) {
    if (g_app.samples.empty()) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<int16_t> pcm(g_app.samples.size());
    for (size_t i = 0; i < g_app.samples.size(); i++) {
        float v = std::max(-1.f, std::min(1.f, g_app.samples[i]));
        pcm[i] = (int16_t)(v * 32767.f);
    }
    uint32_t db = (uint32_t)(pcm.size() * 2);
    auto w16 = [&](uint16_t v){ f.write((char*)&v, 2); };
    auto w32 = [&](uint32_t v){ f.write((char*)&v, 4); };
    f.write("RIFF",4); w32(36+db); f.write("WAVE",4); f.write("fmt ",4);
    w32(16); w16(1); w16(1); w32(SAMPLE_RATE); w32(SAMPLE_RATE*2); w16(2); w16(16);
    f.write("data",4); w32(db);
    f.write((char*)pcm.data(), db);
    return (bool)f;
}

// ── Markdown save ─────────────────────────────────────────────────────────────
static void save_md() {
    if (g_app.tbuf[0] == '\0') { g_app.status = "Nothing to save"; return; }
    const char* filt[] = {"*.md"};
    std::string def = (g_app.folder.empty() ? "." : g_app.folder)
                    + "/" + g_app.take_name + ".md";
    const char* p = tinyfd_saveFileDialog("Save Transcription",
                                          def.c_str(), 1, filt, "Markdown");
    if (!p) return;
    std::ofstream out(p);
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char date[32]; std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", std::localtime(&tt));
    out << "# " << g_app.take_name << "\n\n"
        << "*" << date << " · " << LANG_NAMES[g_app.lang_idx] << "*\n\n"
        << "---\n\n" << g_app.tbuf << "\n";
    g_app.status = std::string("Saved: ") + fs::path(p).filename().string();
}

// ── Folder scan ───────────────────────────────────────────────────────────────
static void scan_folder() {
    g_app.files.clear();
    // Default to current directory if no folder selected
    std::string dir = g_app.folder.empty() ? "." : g_app.folder;
    if (!fs::exists(dir)) return;
    static const char* exts[] = {"wav","mp3","m4a","flac","ogg","mp4","mkv","mov","md"};
    for (auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        if (!ext.empty()) ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        for (auto* x : exts)
            if (ext == x) { g_app.files.push_back(e.path().string()); break; }
    }
    std::sort(g_app.files.begin(), g_app.files.end());
}

// ── Playback ──────────────────────────────────────────────────────────────────
static void play_file(const std::string& path) {
    // Stop current playback
    if (g_app.play_stream) { SDL_DestroyAudioStream(g_app.play_stream); g_app.play_stream = nullptr; }
    if (g_app.play_dev)    { SDL_CloseAudioDevice(g_app.play_dev); g_app.play_dev = 0; }
    g_app.playing = false;

    SDL_AudioSpec src{}; uint8_t* buf = nullptr; uint32_t len = 0;
    if (!SDL_LoadWAV(path.c_str(), &src, &buf, &len)) {
        g_app.status = "Cannot play: " + path; return;
    }
    // Open a playback stream with format conversion to device default
    g_app.play_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &src, nullptr, nullptr);
    if (!g_app.play_stream) { SDL_free(buf); return; }

    SDL_PutAudioStreamData(g_app.play_stream, buf, len);
    SDL_FlushAudioStream(g_app.play_stream);
    SDL_free(buf);

    g_app.play_dev = SDL_GetAudioStreamDevice(g_app.play_stream);
    SDL_ResumeAudioDevice(g_app.play_dev);
    g_app.playing = true;
    g_app.status  = "Playing: " + fs::path(path).filename().string();
}

// ── Model loading ─────────────────────────────────────────────────────────────
static void load_model(const std::string& path) {
    if (g_app.wctx) { whisper_free(g_app.wctx); g_app.wctx = nullptr; }
    g_app.status = "Loading model...";
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    g_app.wctx = whisper_init_from_file_with_params(path.c_str(), cp);
    if (g_app.wctx) {
        g_app.model_path = path;
        g_app.status = "Model: " + fs::path(path).filename().string();
    } else {
        g_app.status = "Failed to load: " + path;
    }
}

// ── Transcription ─────────────────────────────────────────────────────────────
static void do_transcribe() {
    if (!g_app.wctx) { g_app.status = "No model loaded"; return; }
    std::vector<float> samps;
    { std::lock_guard<std::mutex> lk(g_app.mtx); samps = g_app.samples; }
    if (samps.empty()) { g_app.status = "No audio recorded"; return; }

    g_app.state   = RecordState::Transcribing;
    g_app.progress = 0;
    memset(g_app.tbuf, 0, sizeof(g_app.tbuf));

    std::thread([samps]() mutable {
        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        p.beam_search.beam_size = 5;
        p.language              = LANGUAGES[g_app.lang_idx];
        p.print_progress = p.print_realtime = p.print_special = false;
        p.no_context = true;
        p.progress_callback = [](whisper_context*, whisper_state*, int pct, void*){
            g_app.progress = pct;
        };

        whisper_full(g_app.wctx, p, samps.data(), (int)samps.size());

        std::string result; float last_t1 = 0;
        int n = whisper_full_n_segments(g_app.wctx);
        for (int i = 0; i < n; i++) {
            const char* txt  = whisper_full_get_segment_text(g_app.wctx, i);
            float t0         = whisper_full_get_segment_t0(g_app.wctx, i) * 0.01f;
            float prob       = whisper_full_get_segment_no_speech_prob(g_app.wctx, i);
            std::string s = txt;
            while (!s.empty() && s.front() == ' ') s.erase(0, 1);
            if (s.empty()) continue;
            bool brk = (prob > 0.6f) || (!result.empty() && t0 - last_t1 > 1.5f);
            result += (result.empty() ? s : (brk ? "\n\n" + s : " " + s));
            last_t1 = whisper_full_get_segment_t1(g_app.wctx, i) * 0.01f;
        }
        snprintf(g_app.tbuf, sizeof(g_app.tbuf), "%s", result.c_str());
        g_app.state  = RecordState::Done;
        g_app.status = "Done — Ctrl+S or click Save .md";
    }).detach();
}

// ── Recording control ─────────────────────────────────────────────────────────
static void start_rec() {
    { std::lock_guard<std::mutex> lk(g_app.mtx); g_app.samples.clear(); }
    memset(g_app.tbuf, 0, sizeof(g_app.tbuf));
    { time_t t = time(nullptr); struct tm* tm = localtime(&t);
      char buf[32]; strftime(buf, sizeof(buf), "Take %y%m%d %H%M", tm);
      g_app.take_name = buf; }
    SDL_ClearAudioStream(g_app.cap_stream);
    SDL_ResumeAudioDevice(g_app.cap_dev);
    g_app.state  = RecordState::Recording;
    g_app.status = "Recording " + g_app.take_name + "...";
}
static void pause_rec() {
    SDL_PauseAudioDevice(g_app.cap_dev);
    g_app.state  = RecordState::Paused;
    g_app.status = "Paused";
}
static void resume_rec() {
    SDL_ClearAudioStream(g_app.cap_stream);
    SDL_ResumeAudioDevice(g_app.cap_dev);
    g_app.state  = RecordState::Recording;
    g_app.status = "Recording " + g_app.take_name + "...";
}
static void stop_rec() {
    SDL_PauseAudioDevice(g_app.cap_dev);
    g_app.level = 0;
    std::string dir = g_app.folder.empty() ? "." : g_app.folder;
    std::string wp  = dir + "/" + g_app.take_name + ".wav";
    if (save_wav(wp)) { g_app.status = "Saved: " + wp; scan_folder(); }
    else              { g_app.status = "Save failed: " + wp; }
    g_app.state = RecordState::Idle;
    if (g_app.wctx) do_transcribe();
}

// ── Settings panel ────────────────────────────────────────────────────────────
static void draw_settings(float win_h) {
    using namespace RegroovelizerUI;
    const float sw = 300.f;

    const float banner_h = 48.f;
    // Start BELOW the banner — banner stays accessible, hamburger remains clickable
    ImGui::SetNextWindowPos({0, banner_h});
    ImGui::SetNextWindowSize({sw, win_h - banner_h});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(26/255.f, 26/255.f, 26/255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    {12.f, 12.f});
    ImGui::Begin("##NavPanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse);
    ImGui::Spacing();
    renderTitle("SETTINGS");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // Model
    ImGui::TextColored(Colors::TextDim, "WHISPER MODEL");
    ImGui::TextWrapped("%s", g_app.model_path.empty() ? "None" :
                       fs::path(g_app.model_path).filename().string().c_str());
    if (ImGui::Button("Load model...", {sw-16, 32})) {
        const char* f[] = {"*.bin"};
        const char* p = tinyfd_openFileDialog("Select Whisper model",
                            g_app.model_path.c_str(), 1, f, "GGML model", 0);
        if (p) { load_model(p); save_config(); }
        g_app.show_settings = false;
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // Language
    ImGui::TextColored(Colors::TextDim, "LANGUAGE");
    ImGui::SetNextItemWidth(sw-16);
    if (ImGui::BeginCombo("##lang", LANG_NAMES[g_app.lang_idx])) {
        for (int i = 0; i < LANG_COUNT; i++) {
            bool sel = (g_app.lang_idx == i);
            if (ImGui::Selectable(LANG_NAMES[i], sel)) { g_app.lang_idx = i; save_config(); }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // Model download
    ImGui::Spacing();
    ImGui::TextColored(Colors::TextDim, "DOWNLOAD MODEL");
    if (g_app.downloading) {
        float pct  = g_app.download_progress.load();
        size_t mb  = g_app.download_bytes.load() / (1024*1024);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Colors::Red);
        char label[64];
        if (pct >= 0.f) {
            snprintf(label, sizeof(label), "%.0f%%  (%zu MB)", pct*100.f, mb);
            ImGui::ProgressBar(pct, {sw-16, 28}, label);
        } else {
            snprintf(label, sizeof(label),
                     mb > 0 ? "%zu MB received..." : "Connecting...", mb);
            ImGui::ProgressBar(1.f, {sw-16, 28}, label);
        }
        ImGui::PopStyleColor();
        ImGui::TextColored(Colors::TextDim, "%s",
                           fs::path(g_app.download_dest).filename().string().c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f,0.1f,0.1f,1.f));
        if (ImGui::Button("Cancel", {sw-16, 26})) g_app.download_cancel = true;
        ImGui::PopStyleColor();
    } else {
        if (!g_app.download_error.empty()) {
            ImGui::TextColored(Colors::Red, "Error: %s", g_app.download_error.c_str());
            ImGui::TextColored(Colors::TextDim, "Check connection or load a .bin manually.");
            if (!g_app.download_last_url.empty()) {
                if (ImGui::Button("Retry", {sw-16, 28}))
                    download_model(g_app.download_last_url, g_app.download_dest);
            }
            ImGui::Spacing();
        }
        std::string dir = g_app.folder.empty() ? "." : g_app.folder;
        if (ImGui::Button("Base  (~141 MB) — recommended", {sw-16, 28}))
            download_model(
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin",
                dir + "/ggml-base.bin");
        if (ImGui::Button("Tiny  (~39 MB)  — faster, less accurate", {sw-16, 28}))
            download_model(
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin",
                dir + "/ggml-tiny.bin");
        ImGui::Spacing();
        ImGui::TextColored(Colors::TextDim, "If download fails, get the file manually:");
        ImGui::TextColored(Colors::TextDim, "huggingface.co/ggerganov/whisper.cpp");
        float hw = (sw - 20) / 2.f;
        if (ImGui::Button("Copy Base URL", {hw, 24}))
            SDL_SetClipboardText(
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin");
        ImGui::SameLine();
        if (ImGui::Button("Copy Tiny URL", {hw, 24}))
            SDL_SetClipboardText(
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin");
        ImGui::TextColored(Colors::TextDim, "Then use 'Load model...' above.");
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // Input device
    ImGui::TextColored(Colors::TextDim, "INPUT DEVICE");
    ImGui::SetNextItemWidth(sw-16);
    if (ImGui::BeginCombo("##inputdev", g_input_devices.empty() ? "Default" :
                           g_input_devices[g_input_device_idx].c_str())) {
        for (int i = 0; i < (int)g_input_devices.size(); i++) {
            bool sel = (g_input_device_idx == i);
            if (ImGui::Selectable(g_input_devices[i].c_str(), sel)) {
                // Only switch device when not actively recording or monitoring
                bool active = (g_app.state == RecordState::Recording ||
                               g_app.state == RecordState::Paused);
                if (!active && i != g_input_device_idx) {
                    open_capture_device(i);
                    save_config();
                } else if (active) {
                    g_app.status = "Cannot switch device while recording";
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // Folder
    ImGui::TextColored(Colors::TextDim, "RECORDINGS FOLDER");
    ImGui::TextWrapped("%s", g_app.folder.empty() ? "(current directory)" : g_app.folder.c_str());
    if (ImGui::Button("Select folder...", {sw-16, 32})) {
        const char* p = tinyfd_selectFolderDialog("Select folder", g_app.folder.c_str());
        if (p) { g_app.folder = p; scan_folder(); save_config(); }
        g_app.show_settings = false;
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(Colors::TextDim, "Stop recording auto-transcribes");
    ImGui::TextColored(Colors::TextDim, "when a model is loaded.");
    ImGui::Spacing();
    ImGui::TextColored(Colors::TextDim, "Voicenotes  |  by gbraad");
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ── Main render ───────────────────────────────────────────────────────────────
static void render_ui() {
    using namespace RegroovelizerUI;
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;

    // Ctrl+S = save
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_S))
        save_md();

    ImGui::SetNextWindowPos({0,0});
    ImGui::SetNextWindowSize({W,H});
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPosY(56); // below the 48px banner

    bool rec  = (g_app.state == RecordState::Recording);
    bool pau  = (g_app.state == RecordState::Paused);
    bool busy = (g_app.state == RecordState::Transcribing);
    bool done = (g_app.state == RecordState::Done);

    // ── Left: file list ────────────────────────────────────────────────────
    float left_w = std::min(300.f, W * 0.28f);
    float cont_h = H - 56 - 28; // banner + status bar

    // Height breakdown: label(~20) + spacing(~8) + files + button(34) + spacing(~8) = cont_h
    float files_h = cont_h - 80.f;
    ImVec4 btnGray = {42/255.f,42/255.f,42/255.f,1.f};
    ImVec4 btnGrayH= {58/255.f,58/255.f,58/255.f,1.f};
    ImGui::BeginChild("##left", {left_w, cont_h}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Spacing();
    ImGui::TextColored(Colors::TextDim, "FILES");
    ImGui::BeginChild("##files", {left_w - 4, files_h}, true);

    static std::vector<bool> del_armed;
    static int  rename_idx = -1;
    static char rename_buf[256] = {};
    if ((int)del_armed.size() != (int)g_app.files.size()) del_armed.assign(g_app.files.size(), false);

    // name fills whatever is left after del(26) + open(28) + 2 spacings
    float avail_w = ImGui::GetContentRegionAvail().x;
    float spc     = ImGui::GetStyle().ItemSpacing.x;
    float name_w  = avail_w - 26.f - 28.f - spc * 2.f;

    for (int i = 0; i < (int)g_app.files.size(); i++) {
        std::string fname = fs::path(g_app.files[i]).filename().string();
        std::string ext   = fs::path(g_app.files[i]).extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        bool isMd = (ext==".md");

        ImGui::PushID(i);
        bool sel = (g_app.sel_file == i);

        // Filename — click to select, double-click to rename
        if (rename_idx == i) {
            ImGui::SetNextItemWidth(name_w);
            if (ImGui::InputText("##ren", rename_buf, sizeof(rename_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                std::string nf = rename_buf;
                if (!nf.empty()) {
                    auto p = fs::path(g_app.files[i]);
                    if (fs::path(nf).extension().empty()) nf += p.extension().string();
                    fs::path np = p.parent_path() / nf;
                    if (np != p) {
                        std::error_code ec;
                        fs::rename(p, np, ec);
                        if (!ec) scan_folder();
                        else g_app.status = "Rename failed: " + ec.message();
                    }
                }
                rename_idx = -1;
            } else if (ImGui::IsItemDeactivated()) {
                rename_idx = -1; // clicked away — cancel
            }
        } else {
            if (ImGui::Selectable(fname.c_str(), sel, 0, {name_w, 26})) g_app.sel_file = i;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                rename_idx = i;
                strncpy(rename_buf, fname.c_str(), sizeof(rename_buf) - 1);
                rename_buf[sizeof(rename_buf) - 1] = '\0';
            }
        }

        ImGui::SameLine();

        // ✕ delete (two-tap)
        ImGui::PushStyleColor(ImGuiCol_Button,        del_armed[i] ? Colors::Red : btnGray);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, del_armed[i] ? Colors::RedActive : btnGrayH);
        if (ImGui::Button(del_armed[i] ? "?" : "x", {26,26})) {
            if (!del_armed[i]) {
                del_armed[i] = true;
            } else {
                if (g_app.playing && g_app.files[i] == g_app.playback_file) {
                    if (g_app.play_stream) { SDL_DestroyAudioStream(g_app.play_stream); g_app.play_stream=nullptr; }
                    if (g_app.play_dev)    { SDL_CloseAudioDevice(g_app.play_dev); g_app.play_dev=0; }
                    g_app.playing=false;
                }
                if (rename_idx == i) rename_idx = -1;
                fs::remove(g_app.files[i]);
                scan_folder();
                del_armed.clear();
                ImGui::PopStyleColor(2); ImGui::PopID(); break;
            }
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // → load / open
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RedActive);
        if (ImGui::Button("->", {28,26})) {
            g_app.sel_file = i;
            if (!isMd) {
                g_app.take_name = fs::path(g_app.files[i]).stem().string();
                g_app.status = "Selected: " + fname;
            } else {
                g_app.status = "Showing: " + fname;
                std::ifstream f(g_app.files[i]);
                if (f) {
                    std::string line, content;
                    bool inFrontmatter = false, pastHeader = false;
                    while (std::getline(f, line)) {
                        if (!pastHeader && line == "---") { inFrontmatter = !inFrontmatter; continue; }
                        if (inFrontmatter) continue;
                        pastHeader = true;
                        if (!content.empty()) content += "\n";
                        content += line;
                    }
                    snprintf(g_app.tbuf, sizeof(g_app.tbuf), "%s", content.c_str());
                }
            }
        }
        ImGui::PopStyleColor(2);

        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("Refresh", {left_w-8, 28})) scan_folder();
    ImGui::EndChild();
    ImGui::SameLine();

    // ── Right: controls + transcript ───────────────────────────────────────
    float right_w = W - left_w - 16;
    ImGui::BeginChild("##right", {right_w, cont_h});
    ImGui::Spacing();

    // Take + duration
    if (!g_app.take_name.empty()) ImGui::TextColored(Colors::Red, "%s", g_app.take_name.c_str());
    ImGui::SameLine();
    float dur = g_app.samples.empty() ? 0.f : (float)g_app.samples.size() / SAMPLE_RATE;
    if (dur > 0) {
        ImGui::TextColored(Colors::TextDim, "  %02d:%02d", (int)dur/60, (int)dur%60);
    }
    ImGui::Spacing();

    // Level meter during recording/pause
    if (rec || pau) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Colors::Red);
        ImGui::ProgressBar(rec ? std::min(1.f, g_app.level*5.f) : 0.f, {right_w-8, 6}, "");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // ── Buttons row ────────────────────────────────────────────────────────
    float bh = 36.f;

    if (!rec && !pau && !busy) {
        // Idle / Done
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RedActive);
        if (ImGui::Button("Record", {120, bh})) start_rec();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        // Transcribe: needs a model + something recorded
        bool has_audio  = !g_app.samples.empty() || !g_app.take_name.empty();
        bool can_t      = g_app.wctx && has_audio && !busy;
        bool no_model   = !g_app.wctx;
        if (!can_t) ImGui::BeginDisabled();
        if (ImGui::Button("Transcribe", {110, bh})) {
            if (no_model)  g_app.status = "No model loaded — open Settings (hamburger) to load one";
            else           do_transcribe();
        }
        if (!can_t) ImGui::EndDisabled();

        // Play/Stop selected file
        if (g_app.sel_file >= 0 && g_app.sel_file < (int)g_app.files.size()) {
            std::string sx = fs::path(g_app.files[g_app.sel_file]).extension().string();
            std::transform(sx.begin(), sx.end(), sx.begin(), ::tolower);
            if (sx==".wav"||sx==".mp3"||sx==".m4a"||sx==".mp4"||sx==".webm") {
                bool pt = g_app.playing && g_app.files[g_app.sel_file] == g_app.playback_file;
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        pt ? Colors::Red : btnGray);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pt ? Colors::RedActive : btnGrayH);
                if (ImGui::Button(pt ? "Stop" : "Play", {70, bh})) {
                    if (pt) {
                        if (g_app.play_stream) { SDL_DestroyAudioStream(g_app.play_stream); g_app.play_stream=nullptr; }
                        if (g_app.play_dev)    { SDL_CloseAudioDevice(g_app.play_dev); g_app.play_dev=0; }
                        g_app.playing = false; g_app.playback_file.clear();
                    } else {
                        play_file(g_app.files[g_app.sel_file]);
                        g_app.playback_file = g_app.files[g_app.sel_file];
                    }
                }
                ImGui::PopStyleColor(2);
            }
        }

    } else if (rec) {
        if (ImGui::Button("Pause", {110, bh})) pause_rec();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RedActive);
        if (ImGui::Button("Stop", {110, bh})) stop_rec();
        ImGui::PopStyleColor(2);

    } else if (pau) {
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RedActive);
        if (ImGui::Button("Resume", {110, bh})) resume_rec();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Stop", {110, bh})) stop_rec();

    } else if (busy) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Colors::Red);
        int pct = g_app.progress.load();
        if (pct == 0) {
            ImGui::ProgressBar(1.f, {240, bh}, "Initialising...");
        } else {
            ImGui::ProgressBar(pct / 100.f, {240, bh},
                ("Transcribing... " + std::to_string(pct) + "%").c_str());
        }
        ImGui::PopStyleColor();
    }

    // Save .md — visible always, active only when transcript exists
    ImGui::SameLine(0, 12);
    bool can_save = g_app.tbuf[0] != '\0' && !busy;
    if (can_save) {
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RedActive);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Dark);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextDim);
    }
    if (!can_save) ImGui::BeginDisabled();
    if (ImGui::Button("Save .md", {100, bh})) save_md();
    if (!can_save) ImGui::EndDisabled();
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    if (g_app.downloading) {
        float pct = g_app.download_progress.load();
        size_t mb = g_app.download_bytes.load() / (1024*1024);
        char s[80];
        if (pct >= 0.f)
            snprintf(s, sizeof(s), "Downloading: %.0f%%  (%zu MB received)", pct*100.f, mb);
        else
            snprintf(s, sizeof(s), mb > 0 ? "Connecting — %zu MB..." : "Connecting to HuggingFace...", mb);
        g_app.status = s;
    }
    ImGui::TextColored(Colors::TextDim, "%s", g_app.status.c_str());
    ImGui::Spacing();

    // Transcript — InputTextMultiline = selectable + copyable
    float th = cont_h - bh - 70;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f,0.07f,0.07f,1.f));
    ImGui::InputTextMultiline("##transcript", g_app.tbuf, sizeof(g_app.tbuf),
                              {right_w-8, th}, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();

    ImGui::EndChild();

    // Banner always on top
    renderBanner("VOICENOTES", &g_app.show_settings, W);

    ImGui::End(); // close ##root BEFORE opening nav panel so it renders on top

    // Nav panel is a separate top-level window submitted AFTER ##root (same as dizisynth)
    if (g_app.show_settings) {
        draw_settings(H);
        // Click outside the nav panel (right of its 300px width) closes it
        if (ImGui::IsMouseClicked(0) &&
            ImGui::GetIO().MousePos.x > 300.f &&
            ImGui::GetIO().MousePos.y > 48.f) {
            g_app.show_settings = false;
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    SDL_Log("=== Voicenotes Starting ===");

    // Same SDL_Init as junglizer — just VIDEO first, audio via sdl_audio_init()
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s", SDL_GetError());
        return 1;
    }

    // Same GL attributes as junglizer desktop
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g_app.window = SDL_CreateWindow("Voicenotes", 1280, 760,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_app.window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Window: %s", SDL_GetError());
        SDL_Quit(); return 1;
    }

    g_app.gl_context = SDL_GL_CreateContext(g_app.window);
    if (!g_app.gl_context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GL context: %s", SDL_GetError());
        SDL_DestroyWindow(g_app.window); SDL_Quit(); return 1;
    }
    SDL_GL_MakeCurrent(g_app.window, g_app.gl_context);
    SDL_GL_SetSwapInterval(1);

    // ImGui — same as junglizer
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImFontConfig fc; fc.SizePixels = 13.f; fc.OversampleH = 3; fc.OversampleV = 3;
    io.Fonts->AddFontDefault(&fc);

    RegroovelizerUI::setupStyle();

    ImGui_ImplSDL3_InitForOpenGL(g_app.window, g_app.gl_context);
    ImGui_ImplOpenGL2_Init();

    // Audio — same helper as junglizer
    if (!sdl_audio_init()) {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Audio init failed");
        return 1;
    }

    // Config file in SDL pref path (same approach as junglizer)
    char* pref = SDL_GetPrefPath("gbraad", "Voicenotes");
    if (pref) { g_config_path = std::string(pref) + "voicenotes.ini"; SDL_free(pref); }
    load_config();

    // Enumerate inputs, then open saved or default device
    enumerate_inputs();
    open_capture_device(g_input_device_idx);

    // Auto-load model: argv[1] > saved config > common filenames
    auto try_load = [](const char* p){ if (!g_app.wctx && fs::exists(p)) load_model(p); };
    if (argc > 1) try_load(argv[1]);
    if (!g_app.model_path.empty()) load_model(g_app.model_path); // from config
    try_load("ggml-base.bin");
    try_load("ggml-tiny.bin");

    // Initial file list scan (current dir if no folder saved)
    scan_folder();

    // Main loop — same structure as junglizer
    while (g_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) g_running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q &&
                (e.key.mod & SDL_KMOD_CTRL)) g_running = false;
        }

        // Check playback: stream drains automatically, just track state
        if (g_app.playing && g_app.play_stream) {
            int avail = SDL_GetAudioStreamAvailable(g_app.play_stream);
            if (avail == 0) {
                SDL_DestroyAudioStream(g_app.play_stream); g_app.play_stream = nullptr;
                SDL_CloseAudioDevice(g_app.play_dev);      g_app.play_dev = 0;
                g_app.playing = false;
            }
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        render_ui();
        ImGui::Render();

        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(g_app.window);
    }

    // Cleanup
    save_config();
    if (g_app.state == RecordState::Recording || g_app.state == RecordState::Paused)
        stop_rec();

    if (g_app.wctx)        whisper_free(g_app.wctx);
    if (g_app.cap_stream)  SDL_DestroyAudioStream(g_app.cap_stream);
    if (g_app.play_stream) SDL_DestroyAudioStream(g_app.play_stream);

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(g_app.gl_context);
    SDL_DestroyWindow(g_app.window);
    SDL_Quit();

    SDL_Log("=== Voicenotes exiting cleanly ===");
    return 0;
}
