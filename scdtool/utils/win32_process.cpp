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

namespace {
	std::vector<uint8_t> run_process_capture_one_stream(const std::filesystem::path& exe, const std::vector<std::wstring>& args, bool captureStderr);
}

std::vector<uint8_t> run_process_capture_stdout(const std::filesystem::path& exe, const std::vector<std::wstring>& args) {
	return run_process_capture_one_stream(exe, args, false);
}

std::vector<uint8_t> run_process_capture_stderr(const std::filesystem::path& exe, const std::vector<std::wstring>& args) {
	return run_process_capture_one_stream(exe, args, true);
}

namespace {
	std::vector<uint8_t> run_process_capture_one_stream(const std::filesystem::path& exe, const std::vector<std::wstring>& args, bool captureStderr) {
	SECURITY_ATTRIBUTES sa{.nLength = sizeof(sa), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};

	auto_handle stdoutRead, stdoutWrite;
	if (!CreatePipe(&stdoutRead.Value, &stdoutWrite.Value, &sa, 0))
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreatePipe");
	if (!SetHandleInformation(stdoutRead.Value, HANDLE_FLAG_INHERIT, 0))
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "SetHandleInformation");

	auto_handle nulInput, nulOut, nulErr;
	nulInput.Value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (nulInput.Value == INVALID_HANDLE_VALUE)
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateFileW(NUL, read)");
	for (auto* h : {&nulOut, &nulErr}) {
		h->Value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
		if (h->Value == INVALID_HANDLE_VALUE)
			throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateFileW(NUL, write)");
	}

	std::wstring cmdLine = quote_argument(exe.wstring());
	for (const auto& arg : args) {
		cmdLine.push_back(L' ');
		cmdLine += quote_argument(arg);
	}

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nulInput.Value;
	si.hStdOutput = captureStderr ? nulOut.Value : stdoutWrite.Value;
	si.hStdError = captureStderr ? stdoutWrite.Value : nulErr.Value;

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

	const auto_handle hProcess{pi.hProcess}, hThread{pi.hThread};
	stdoutWrite.reset();
	nulInput.reset();
	nulOut.reset();
	nulErr.reset();

	std::vector<uint8_t> result;
	uint8_t buf[65536];
	DWORD read;
	while (ReadFile(stdoutRead.Value, buf, sizeof(buf), &read, nullptr) && read > 0)
		result.insert(result.end(), buf, buf + read);
	stdoutRead.reset();

	WaitForSingleObject(hProcess.Value, INFINITE);
	DWORD exitCode = 0;
	GetExitCodeProcess(hProcess.Value, &exitCode);
	if (exitCode != 0) {
		// Whatever the child said about why is in the stream we captured; a bare exit code
		// leaves the caller guessing, and for a tool that reports its own diagnosis (llogg
		// prints how far off the encode was) that is the whole message. Keep the tail,
		// stripped to printable ASCII so a binary stdout cannot wreck the console.
		std::string tail;
		const auto keep = (std::min<size_t>)(result.size(), 2048);
		for (auto c : std::span(result).last(keep)) {
			if (c == '\r')
				continue;
			tail.push_back(c == '\n' || (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?');
		}
		while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' '))
			tail.pop_back();
		throw std::runtime_error(std::format("{} exited with code {}{}",
			xivres::util::unicode::convert<std::string>(exe.wstring()), exitCode,
			tail.empty() ? std::string() : std::format(":\n{}", tail)));
	}

	return result;
	}
}
