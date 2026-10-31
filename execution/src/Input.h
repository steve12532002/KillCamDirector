#pragma once

namespace ED::Input
{
	void        Install();
	void        BeginCapture();
	void        CancelCapture();
	bool        IsCapturing();
	const char* KeyName(std::uint32_t a_code);
}
