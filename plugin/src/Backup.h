#pragma once

// Outpost backup store + Papyrus natives (script OSB_Native).
namespace OSB
{
	// Called by the landing hook: returns true and the target (degrees) if a landing is armed; disarms it.
	bool TakeArmedLanding(double& a_latDeg, double& a_lonDeg);

	// Register the OSB_Native functions with the Papyrus VM (call after the VM exists).
	void RegisterNatives();
}
