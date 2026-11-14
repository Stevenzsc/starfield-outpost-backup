#pragma once

#include <filesystem>

// Outpost backup store + Papyrus natives (script OSB_Native).
namespace OSB
{
	// Called by the landing hook: returns true and the target (degrees) if a landing is armed for a_planet; disarms it.
	bool TakeArmedLanding(std::uint32_t a_planet, double& a_latDeg, double& a_lonDeg);

	// DocumentsMy GamesStarfieldOutpostBackup
	std::filesystem::path DataFolder();

	// Register the OSB_Native functions with the Papyrus VM (call after the VM exists).
	void RegisterNatives();
}
