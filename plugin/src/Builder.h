#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Builds objects through the game's own workshop engine (like the player does), so habs get their
// doors, walls, roofs, foundations and terrain flattening. See docs/re-workshop.md.
namespace OSB::Builder
{
	struct Job
	{
		std::uint32_t formID{ 0 };   // base object (runtime form ID)
		float x{}, y{}, z{};
		float yawDeg{};              // ObjectReference.GetAngleZ()
		std::string name;            // for the log
		float size{};                // footprint (m^2) from the base bounds; bigger objects are built first
	};

	// Install the engine hooks (call once at plugin load, before the game runs).
	void Install();

	// Start building the jobs on a worker thread. The player must be in build mode at the outpost.
	// Returns false if a build is already running.
	bool Start(std::vector<Job> a_jobs);

	// Progress: [done, built, failed, total, running(0/1)]
	std::vector<std::int32_t> Progress();

	// Ask the worker to stop after the current object.
	void Stop();
}
