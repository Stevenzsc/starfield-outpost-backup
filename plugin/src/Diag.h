#pragma once

#include <filesystem>

namespace OSB::Diag
{
	// Log workshop placement events and their call stacks if a_flagFile exists.
	void Install(const std::filesystem::path& a_flagFile);
}
