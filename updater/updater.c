#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <time.h>

#include "cJSON.h"
#include "version.h"

/* ========================= Helpers ========================= */

static int parse_url(const char *url, wchar_t *host, int host_sz,
                     wchar_t *path, int path_sz,
                     INTERNET_PORT *port, int *secure) {
    wchar_t wurl[1024];
    mbstowcs(wurl, url, 1024);

    URL_COMPONENTSW uc = {0};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = -1;
    uc.dwUrlPathLength = -1;

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;

    wcsncpy(host, uc.lpszHostName, host_sz - 1);
    host[uc.dwHostNameLength] = 0;

    wcsncpy(path, uc.lpszUrlPath, path_sz - 1);
    path[uc.dwUrlPathLength] = 0;

    *port = uc.nPort;
    *secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return 1;
}

/* ========================= HTTP GET ========================= */

static char *http_get(const char *url, const char *accept) {
    wchar_t host[256], path[512];
    INTERNET_PORT port;
    int secure;

    if (!parse_url(url, host, 256, path, 512, &port, &secure)) return NULL;

    HINTERNET hSession = WinHttpOpen(L"Lulu-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return NULL;

    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return NULL; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    /* Add Accept + User-Agent headers */
    wchar_t hdrs[512];
    char ha[256];
    snprintf(ha, sizeof(ha), "Accept: %s\r\n", accept ? accept : "application/json");
    mbstowcs(hdrs, ha, 512);
    WinHttpAddRequestHeaders(hRequest, hdrs, -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto fail;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto fail;

    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &status, &slen, NULL);
    if (status != 200) goto fail;

    int cap = 65536, total = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) goto fail;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (avail == 0) break;
        if (total + (int)avail >= cap) {
            cap *= 2;
            if (cap > 8 * 1024 * 1024) break;
            buf = (char *)realloc(buf, cap);
            if (!buf) goto fail;
        }
        DWORD downloaded = 0;
        if (!WinHttpReadData(hRequest, buf + total, avail, &downloaded)) break;
        total += downloaded;
    }
    buf[total] = 0;

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return buf;

fail:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return NULL;
}

/* ========================= HTTP Download to file ========================= */

static int http_download(const char *url, const char *dest_path) {
    wchar_t host[256], wpath[512];
    INTERNET_PORT port;
    int secure;

    if (!parse_url(url, host, 256, wpath, 512, &port, &secure)) return 0;

    HINTERNET hSession = WinHttpOpen(L"Lulu-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    WinHttpSetTimeouts(hSession, 15000, 15000, 60000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    /* Follow redirects (GitHub releases redirect to CDN) */
    DWORD opt_flags = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
        &opt_flags, sizeof(opt_flags));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto fail;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto fail;

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) goto fail;

    char chunk[65536];
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (avail == 0) break;
        DWORD to_read = avail > sizeof(chunk) ? sizeof(chunk) : avail;
        DWORD downloaded = 0;
        if (!WinHttpReadData(hRequest, chunk, to_read, &downloaded)) break;
        fwrite(chunk, 1, downloaded, fp);
    }
    fclose(fp);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 1;

fail:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}

/* ========================= Version Parsing ========================= */

static int version_parse(const char *tag) {
    /* "v4.1.0" or "4.1.0" → 40100 */
    int major = 0, minor = 0, patch = 0;
    const char *p = tag;
    if (*p == 'v' || *p == 'V') p++;
    if (sscanf(p, "%d.%d.%d", &major, &minor, &patch) != 3) return -1;
    if (major < 0 || minor < 0 || patch < 0) return -1;
    return major * 10000 + minor * 100 + patch;
}

static int version_compare(int remote, int local) {
    if (remote > local) return 1;
    if (remote < local) return -1;
    return 0;
}

/* ========================= Zip Extraction ========================= */

static int extract_zip(const char *zip_path, const char *dest_dir) {
    /* Use built-in Windows tar (Windows 10 1803+) */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", zip_path, dest_dir);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return 0;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return (wait == WAIT_OBJECT_0);
}

/* ========================= File Copy (preserving user data) ========================= */

static const char *PRESERVED_FILES[] = {
    "config.json",
    "install.json",
    NULL
};

static const char *PRESERVED_DIRS[] = {
    "state",
    "workspace",
    "tg_data",
    NULL
};

static int is_preserved(const char *name) {
    for (int i = 0; PRESERVED_FILES[i]; i++) {
        if (_stricmp(name, PRESERVED_FILES[i]) == 0) return 1;
    }
    for (int i = 0; PRESERVED_DIRS[i]; i++) {
        if (_stricmp(name, PRESERVED_DIRS[i]) == 0) return 1;
    }
    return 0;
}

static int update_copy_files(const char *src_dir, const char *dst_dir) {
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", src_dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int ok = 1;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (is_preserved(fd.cFileName)) {
            printf("  [SKIP] %s (preserved)\n", fd.cFileName);
            continue;
        }

        char src_path[MAX_PATH], dst_path[MAX_PATH];
        snprintf(src_path, MAX_PATH, "%s\\%s", src_dir, fd.cFileName);
        snprintf(dst_path, MAX_PATH, "%s\\%s", dst_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* Create destination subdir, then recurse */
            CreateDirectoryA(dst_path, NULL);
            /* For directories, copy contents recursively */
            char sub_pattern[MAX_PATH];
            snprintf(sub_pattern, MAX_PATH, "%s\\*", src_path);
            WIN32_FIND_DATAA sfd;
            HANDLE hSub = FindFirstFileA(sub_pattern, &sfd);
            if (hSub != INVALID_HANDLE_VALUE) {
                do {
                    if (sfd.cFileName[0] == '.') continue;
                    char sp[MAX_PATH], dp[MAX_PATH];
                    snprintf(sp, MAX_PATH, "%s\\%s", src_path, sfd.cFileName);
                    snprintf(dp, MAX_PATH, "%s\\%s", dst_path, sfd.cFileName);
                    if (!CopyFileA(sp, dp, FALSE)) {
                        printf("  [WARN] Failed to copy %s\n", sp);
                        ok = 0;
                    } else {
                        printf("  [COPY] %s\n", sfd.cFileName);
                    }
                } while (FindNextFileA(hSub, &sfd));
                FindClose(hSub);
            }
        } else {
            if (!CopyFileA(src_path, dst_path, FALSE)) {
                printf("  [WARN] Failed to copy %s\n", fd.cFileName);
                ok = 0;
            } else {
                printf("  [COPY] %s\n", fd.cFileName);
            }
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return ok;
}

/* ========================= Process Management ========================= */

static DWORD find_running_agent(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = {0};
    pe.dwSize = sizeof(pe);

    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "agent.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
}

static int stop_agent(DWORD pid) {
    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) return 0;

    /* Try WM_CLOSE first via PostThreadMessage */
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {0};
        te.dwSize = sizeof(te);
        if (Thread32First(hSnap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) {
                    PostThreadMessage(te.th32ThreadID, WM_CLOSE, 0, 0);
                }
            } while (Thread32Next(hSnap, &te));
        }
        CloseHandle(hSnap);
    }

    /* Wait up to 10 seconds */
    if (WaitForSingleObject(hProc, 10000) == WAIT_OBJECT_0) {
        CloseHandle(hProc);
        return 1;
    }

    /* Force terminate */
    TerminateProcess(hProc, 1);
    WaitForSingleObject(hProc, 3000);
    CloseHandle(hProc);
    return 1;
}

/* ========================= Install Dir Detection ========================= */

static void get_install_dir(char *dir, int sz, const char *override) {
    if (override) {
        strncpy(dir, override, sz - 1);
        dir[sz - 1] = 0;
        return;
    }

    /* Try registry first */
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Lulu", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type, len = sz;
        if (RegQueryValueExA(hKey, "InstallDir", NULL, &type, (LPBYTE)dir, &len) == ERROR_SUCCESS && type == REG_SZ) {
            RegCloseKey(hKey);
            return;
        }
        RegCloseKey(hKey);
    }

    /* Fallback: directory of this exe */
    GetModuleFileNameA(NULL, dir, sz);
    char *sep = strrchr(dir, '\\');
    if (sep) *sep = 0;
}

/* ========================= --check Mode ========================= */

static int do_check(const char *install_dir) {
    printf("[CHECK] Querying GitHub for latest release...\n");

    char *response = http_get(LULU_UPDATE_URL, "application/vnd.github+json");
    if (!response) {
        printf("[ERROR] Failed to reach GitHub API\n");
        /* Write error result */
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        char out_path[MAX_PATH];
        snprintf(out_path, MAX_PATH, "%slulu_update_check.json", tmp);
        FILE *fp = fopen(out_path, "w");
        if (fp) {
            fprintf(fp, "{\"has_update\":false,\"remote_version\":\"\",\"download_url\":\"\","
                        "\"message\":\"Failed to contact GitHub API\"}\n");
            fclose(fp);
        }
        return 1;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) {
        printf("[ERROR] Invalid JSON from GitHub\n");
        return 1;
    }

    /* Parse release info */
    cJSON *tag_j = cJSON_GetObjectItem(root, "tag_name");
    cJSON *html_j = cJSON_GetObjectItem(root, "html_url");
    if (!cJSON_IsString(tag_j)) {
        printf("[ERROR] No tag_name in release\n");
        cJSON_Delete(root);
        return 1;
    }

    const char *tag = tag_j->valuestring;
    int remote_ver = version_parse(tag);
    if (remote_ver < 0) {
        printf("[ERROR] Cannot parse remote version: %s\n", tag);
        cJSON_Delete(root);
        return 1;
    }

    int local_ver = LULU_VERSION_NUMBER;
    int cmp = version_compare(remote_ver, local_ver);
    int has_update = (cmp > 0);

    /* Find win64 zip asset */
    const char *download_url = "";
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (cJSON_IsArray(assets)) {
        int n = cJSON_GetArraySize(assets);
        for (int i = 0; i < n; i++) {
            cJSON *asset = cJSON_GetArrayItem(assets, i);
            cJSON *name = cJSON_GetObjectItem(asset, "name");
            cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
            if (cJSON_IsString(name) && cJSON_IsString(url)) {
                /* Match lulu-*-win64.zip */
                if (strstr(name->valuestring, "win64") && strstr(name->valuestring, ".zip")) {
                    download_url = url->valuestring;
                    break;
                }
            }
        }
    }

    /* Write check result to temp file */
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char out_path[MAX_PATH];
    snprintf(out_path, MAX_PATH, "%slulu_update_check.json", tmp);

    FILE *fp = fopen(out_path, "w");
    if (fp) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "has_update", has_update);
        cJSON_AddStringToObject(out, "remote_version", tag);
        cJSON_AddStringToObject(out, "download_url", download_url);

        char msg[256];
        if (has_update) {
            snprintf(msg, sizeof(msg), "Update available: v%s -> %s", LULU_VERSION_STR, tag);
        } else {
            snprintf(msg, sizeof(msg), "Already up to date (v%s)", LULU_VERSION_STR);
        }
        cJSON_AddStringToObject(out, "message", msg);

        char *json_str = cJSON_PrintUnformatted(out);
        fprintf(fp, "%s\n", json_str);
        free(json_str);
        cJSON_Delete(out);
        fclose(fp);
    }

    printf("[CHECK] %s\n", has_update ?
        "Update available" : "Already up to date");
    printf("  Local:  v%s (%d)\n", LULU_VERSION_STR, local_ver);
    printf("  Remote: %s (%d)\n", tag, remote_ver);
    if (has_update) {
        printf("  URL:    %s\n", download_url);
    }

    cJSON_Delete(root);
    return 0;
}

/* ========================= --apply Mode ========================= */

static int do_apply(const char *install_dir, int restart) {
    /* Read check result */
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char check_path[MAX_PATH];
    snprintf(check_path, MAX_PATH, "%slulu_update_check.json", tmp);

    FILE *fp = fopen(check_path, "r");
    if (!fp) {
        printf("[ERROR] No check result found. Run --check first.\n");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *check_data = (char *)malloc(fsize + 1);
    fread(check_data, 1, fsize, fp);
    check_data[fsize] = 0;
    fclose(fp);

    cJSON *check = cJSON_Parse(check_data);
    free(check_data);
    if (!check) {
        printf("[ERROR] Invalid check result JSON\n");
        return 1;
    }

    cJSON *has_update_j = cJSON_GetObjectItem(check, "has_update");
    cJSON *dl_url_j = cJSON_GetObjectItem(check, "download_url");
    cJSON *remote_ver_j = cJSON_GetObjectItem(check, "remote_version");

    if (!cJSON_IsTrue(has_update_j)) {
        const char *msg = "No update available";
        cJSON *msg_j = cJSON_GetObjectItem(check, "message");
        if (cJSON_IsString(msg_j)) msg = msg_j->valuestring;
        printf("[APPLY] %s\n", msg);
        cJSON_Delete(check);
        return 0;
    }

    if (!cJSON_IsString(dl_url_j) || dl_url_j->valuestring[0] == 0) {
        printf("[ERROR] No download URL in check result (no win64.zip asset found)\n");
        cJSON_Delete(check);
        return 1;
    }

    const char *download_url = dl_url_j->valuestring;
    const char *remote_tag = cJSON_IsString(remote_ver_j) ? remote_ver_j->valuestring : "unknown";

    printf("[APPLY] Downloading %s ...\n", remote_tag);

    /* Download zip to temp */
    char zip_path[MAX_PATH];
    snprintf(zip_path, MAX_PATH, "%slulu-update.zip", tmp);

    if (!http_download(download_url, zip_path)) {
        printf("[ERROR] Download failed\n");
        cJSON_Delete(check);
        return 1;
    }
    printf("[APPLY] Download complete: %s\n", zip_path);

    /* Extract */
    char extract_dir[MAX_PATH];
    snprintf(extract_dir, MAX_PATH, "%slulu-update\\", tmp);
    CreateDirectoryA(extract_dir, NULL);

    printf("[APPLY] Extracting...\n");
    if (!extract_zip(zip_path, extract_dir)) {
        printf("[ERROR] Extraction failed\n");
        cJSON_Delete(check);
        return 1;
    }

    /* The zip might contain a top-level directory (lulu-v4.1.0-win64/).
       Find the actual content directory. */
    char content_dir[MAX_PATH];
    strncpy(content_dir, extract_dir, MAX_PATH);

    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s*", extract_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        int dir_count = 0;
        char last_dir[MAX_PATH] = {0};
        do {
            if (fd.cFileName[0] == '.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                dir_count++;
                strncpy(last_dir, fd.cFileName, MAX_PATH);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);

        /* If exactly one subdir, assume that's the content root */
        if (dir_count == 1) {
            snprintf(content_dir, MAX_PATH, "%s%s\\", extract_dir, last_dir);
        }
    }

    /* Stop running agent */
    DWORD agent_pid = find_running_agent();
    if (agent_pid && agent_pid != GetCurrentProcessId()) {
        printf("[APPLY] Stopping agent.exe (PID %lu)...\n", agent_pid);
        stop_agent(agent_pid);
        Sleep(1000);
    }

    /* Copy files */
    printf("[APPLY] Installing to %s\n", install_dir);
    if (!update_copy_files(content_dir, install_dir)) {
        printf("[WARN] Some files failed to copy\n");
    }

    /* Write install.json with new version */
    char install_json_path[MAX_PATH];
    snprintf(install_json_path, MAX_PATH, "%s\\install.json", install_dir);
    fp = fopen(install_json_path, "w");
    if (fp) {
        fprintf(fp, "{\"version\":\"%s\",\"installed_at\":%lld}\n",
            remote_tag, (long long)time(NULL));
        fclose(fp);
    }

    /* Cleanup temp files */
    DeleteFileA(zip_path);
    /* Leave extracted dir for debugging */

    printf("[APPLY] Update complete: %s\n", remote_tag);

    /* Optionally restart */
    if (restart) {
        char agent_path[MAX_PATH];
        snprintf(agent_path, MAX_PATH, "%s\\agent.exe", install_dir);
        printf("[APPLY] Restarting agent...\n");

        STARTUPINFOA si = {0};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {0};
        CreateProcessA(agent_path, NULL, NULL, NULL, FALSE,
            DETACHED_PROCESS, NULL, install_dir, &si, &pi);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    cJSON_Delete(check);
    return 0;
}

/* ========================= Main ========================= */

static void print_usage(void) {
    printf("Lulu Updater v%s\n\n", LULU_VERSION_STR);
    printf("Usage:\n");
    printf("  updater.exe --check [--install-dir <path>]\n");
    printf("  updater.exe --apply [--install-dir <path>] [--restart]\n");
    printf("  updater.exe --version\n");
}

int main(int argc, char *argv[]) {
    int mode = 0; /* 0=none, 1=check, 2=apply */
    const char *install_dir_override = NULL;
    int restart = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) mode = 1;
        else if (strcmp(argv[i], "--apply") == 0) mode = 2;
        else if (strcmp(argv[i], "--install-dir") == 0 && i + 1 < argc)
            install_dir_override = argv[++i];
        else if (strcmp(argv[i], "--restart") == 0) restart = 1;
        else if (strcmp(argv[i], "--version") == 0) {
            printf("Lulu Updater v%s (build %d)\n", LULU_VERSION_FULL, LULU_VERSION_NUMBER);
            return 0;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    if (mode == 0) {
        print_usage();
        return 1;
    }

    char install_dir[MAX_PATH];
    get_install_dir(install_dir, MAX_PATH, install_dir_override);
    printf("Install dir: %s\n", install_dir);

    if (mode == 1) return do_check(install_dir);
    if (mode == 2) return do_apply(install_dir, restart);

    return 1;
}
