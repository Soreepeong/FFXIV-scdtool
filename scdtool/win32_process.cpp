#include "pch.h"
#include "win32_process.h"

namespace {
	struct auto_handle {
		HANDLE Value = nullptr;

		auto_handle() = default;
		explicit auto_handle(HANDLE h) : Value(h) {}
		auto_handle(const auto_handle&) = delete;
		auto_handle& operator=(const auto_handle&) = delete;

		auto_handle(auto_handle&& r) noexcept : Value(r.Value) { r.Value = nullptr; }

		auto_handle& operator=(auto_handle&& r) noexcept {
			if (this != &r) {
				reset();
				Value = r.Value;
				r.Value = nullptr;
			}
			return *this;
		}

		~auto_handle() { reset(); }

		void reset() {
			if (Value && Value != INVALID_HANDLE_VALUE)
				CloseHandle(Value);
			Value = nullptr;
		}
	};

	std::wstring quote_argument(const std::wstring& arg) {
		if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
			return arg;

		std::wstring res(1, L'"');
		for (auto it = arg.begin(); ; ++it) {
			size_t backslashCount = 0;
			while (it != arg.end() && *it == L'\\') {
				++it;
				++backslashCount;
			}
			if (it == arg.end()) {
				res.append(backslashCount * 2, L'\\');
				break;
			} else if (*it == L'"') {
				res.append(backslashCount * 2 + 1, L'\\');
				res.push_back(*it);
			} else {
				res.append(backslashCount, L'\\');
				res.push_back(*it);
			}
		}
		res.push_back(L'"');
		return res;
	}
}

std::vector<uint8_t> run_process_capture_stdout(const std::filesystem::path& exe, const std::vector<std::wstring>& args) {
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

	auto_handle stdoutRead, stdoutWrite;
	if (!CreatePipe(&stdoutRead.Value, &stdoutWrite.Value, &sa, 0))
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreatePipe");
	if (!SetHandleInformation(stdoutRead.Value, HANDLE_FLAG_INHERIT, 0))
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "SetHandleInformation");

	auto_handle nulInput, nulError;
	nulInput.Value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (nulInput.Value == INVALID_HANDLE_VALUE)
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateFileW(NUL, read)");
	nulError.Value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (nulError.Value == INVALID_HANDLE_VALUE)
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateFileW(NUL, write)");

	std::wstring cmdLine = quote_argument(exe.wstring());
	for (const auto& arg : args) {
		cmdLine.push_back(L' ');
		cmdLine += quote_argument(arg);
	}

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nulInput.Value;
	si.hStdOutput = stdoutWrite.Value;
	si.hStdError = nulError.Value;

	PROCESS_INFORMATION pi{};
	const auto created = CreateProcessW(
		nullptr, // let Windows resolve `exe` (possibly a bare name like L"ffmpeg") via the standard search path, including PATH
		cmdLine.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_NO_WINDOW,
		nullptr,
		nullptr,
		&si,
		&pi);
	if (!created)
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), std::format("CreateProcessW({})", xivres::util::unicode::convert<std::string>(exe.wstring())));

	auto_handle hProcess{pi.hProcess}, hThread{pi.hThread};
	stdoutWrite.reset();
	nulInput.reset();
	nulError.reset();

	std::vector<uint8_t> result;
	uint8_t buf[65536];
	DWORD read;
	while (ReadFile(stdoutRead.Value, buf, sizeof(buf), &read, nullptr) && read > 0)
		result.insert(result.end(), buf, buf + read);
	stdoutRead.reset();

	WaitForSingleObject(hProcess.Value, INFINITE);
	DWORD exitCode = 0;
	GetExitCodeProcess(hProcess.Value, &exitCode);
	if (exitCode != 0)
		throw std::runtime_error(std::format("{} exited with code {}", xivres::util::unicode::convert<std::string>(exe.wstring()), exitCode));

	return result;
}
