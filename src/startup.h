// AnkhCore 2.5 — start with Windows.
// A per-user scheduled task \AnkhCore\Startup with a logon trigger starts AnkhCore at sign-in with
// the same rights as the existing \AnkhCore\Launch task (no UAC prompt, no service, no extra process).
// Turning the option off deletes the task. The UI always shows the state read back from Task Scheduler.
#pragma once
#include "base.h"

namespace ak {

constexpr wchar_t kStartupTaskName[] = L"AnkhCore\\Startup";

// Task definition (UTF-16 XML for schtasks /Create /XML). Pure: unit-tested.
inline std::wstring StartupTaskXml(const std::wstring& sid, const std::wstring& exe, const std::wstring& workDir, int delaySeconds = 20) {
    return
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Author>AnkhCore</Author><Description>Starts AnkhCore at sign-in (AnkhCore > Startup &amp; Background).</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId><Delay>PT" + std::to_wstring(delaySeconds) + L"S</Delay></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + exe + L"\"</Command><Arguments>--elevated --startup</Arguments><WorkingDirectory>" + workDir + L"</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n";
}
// True when an exported task definition is a valid AnkhCore startup task for this exe.
inline bool StartupXmlValid(const std::wstring& xml, const std::wstring& exe) {
    const std::wstring x = Lower(xml);
    return x.find(L"<logontrigger") != std::wstring::npos && x.find(L"<enabled>false</enabled>") == std::wstring::npos &&
           x.find(Lower(exe)) != std::wstring::npos && x.find(L"--startup") != std::wstring::npos;
}

// Reads the real state from Task Scheduler (read-only query).
inline bool StartupTaskRegistered() {
    std::wstring xml;
    const DWORD rc = RunCapture(SysDir() + L"\\schtasks.exe", L"/Query /TN \"" + std::wstring(kStartupTaskName) + L"\" /XML",
                                [&](const std::wstring& l) { xml += l + L"\n"; }, 15000);
    return rc == 0 && StartupXmlValid(xml, AppExe());
}

// Registers or removes the task, then reads it back. Returns true only when the read-back matches.
inline bool SetStartWithWindows(bool on) {
    if (on) {
        const std::wstring sid = UserSid();
        if (sid.empty()) { LogE(L"Start with Windows: the current user could not be identified."); return false; }
        const std::wstring xml = StartupTaskXml(sid, AppExe(), AppDir());
        const std::wstring tmp = DataDir() + L"\\startup-task.xml";
        std::vector<uint8_t> b{0xFF, 0xFE};
        for (wchar_t c : xml) { b.push_back(static_cast<uint8_t>(c)); b.push_back(static_cast<uint8_t>(c >> 8)); }
        if (!WriteAll(tmp, b.data(), b.size())) { LogE(L"Start with Windows: the task definition could not be written."); return false; }
        const DWORD rc = Run(SysDir() + L"\\schtasks.exe", L"/Create /TN \"" + std::wstring(kStartupTaskName) + L"\" /XML \"" + tmp + L"\" /F", true, 30000);
        DeleteFileW(tmp.c_str());
        if (rc != 0) { LogE(L"Start with Windows: Task Scheduler refused the task (schtasks " + std::to_wstring(rc) + L")."); return false; }
    } else {
        const DWORD rc = Run(SysDir() + L"\\schtasks.exe", L"/Delete /TN \"" + std::wstring(kStartupTaskName) + L"\" /F", true, 15000);
        if (rc != 0 && StartupTaskRegistered()) { LogE(L"Start with Windows: the startup task could not be removed (schtasks " + std::to_wstring(rc) + L")."); return false; }
    }
    const bool now = StartupTaskRegistered();
    if (now != on) { LogE(std::wstring(L"Start with Windows: Task Scheduler reports it is still ") + (now ? L"on." : L"off.")); return false; }
    LogOk(on ? L"AnkhCore will start when you sign in (task \\AnkhCore\\Startup, verified)." : L"AnkhCore will not start with Windows (startup task removed, verified).");
    return true;
}

}  // namespace ak
