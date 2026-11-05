// A task that starts us as the user signs in. Windows runs its startup list one app at a time, minutes after sign-in on a busy
// machine; a logon task doesn't wait in that line. The startup entry stays as the switch the user sees, and as the way in if this fails.
#include <windows.h>
#include <comdef.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <string>
#include <thread>

#pragma comment(lib, "taskschd.lib")

namespace sys {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring Escaped(const std::wstring& s) { // a path going into xml
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'&') out += L"&amp;";
        else if (c == L'<') out += L"&lt;";
        else if (c == L'>') out += L"&gt;";
        else out += c;
    }
    return out;
}

std::wstring UserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    BYTE buf[256];
    DWORD size = sizeof(buf);
    std::wstring out;
    if (GetTokenInformation(token, TokenUser, buf, size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

std::wstring TaskXml(const std::wstring& sid, const std::wstring& exe, const std::wstring& args) {
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
           L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
           L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>"
           L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
           L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
           L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>" // 4: a task's default of 7 runs below normal
           L"<Actions Context=\"Author\"><Exec><Command>" + Escaped(exe) + L"</Command><Arguments>" + Escaped(args) + L"</Arguments><WorkingDirectory>" + Escaped(dir) + L"</WorkingDirectory></Exec></Actions>"
           L"</Task>";
}

void Apply(const std::wstring& name, const std::wstring& exe, const std::wstring& args, bool on) {
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service))) ||
        FAILED(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t())) || FAILED(service->GetFolder(_bstr_t(L"\\"), &root)))
        return;
    ComPtr<IRegisteredTask> have;
    bool exists = SUCCEEDED(root->GetTask(_bstr_t(name.c_str()), &have)) && have;
    if (!on) {
        if (exists) root->DeleteTask(_bstr_t(name.c_str()), 0);
        return;
    }
    std::wstring sid = UserSid();
    if (sid.empty()) return;
    std::wstring xml = TaskXml(sid, exe, args);
    if (exists) { // already right: nothing written
        BSTR now = nullptr;
        bool same = SUCCEEDED(have->get_Xml(&now)) && now && wcsstr(now, (L"<Command>" + Escaped(exe) + L"</Command>").c_str());
        SysFreeString(now);
        if (same) return;
    }
    ComPtr<IRegisteredTask> made;
    root->RegisterTask(_bstr_t(name.c_str()), _bstr_t(xml.c_str()), TASK_CREATE_OR_UPDATE, _variant_t(), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(), &made);
}

}

void LogonTask(const std::wstring& name, const std::wstring& exe, const std::wstring& args, bool on) { // off its own thread: the scheduler can take seconds to answer at sign-in
    std::thread([name, exe, args, on] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        Apply(name, exe, args, on);
        CoUninitialize();
    }).detach();
}

}
