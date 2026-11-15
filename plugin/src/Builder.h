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
		std::uint32_t order{ 0 };    // reference form ID at backup time (rises in build order), 0 if unknown
		float axDeg{}, ayDeg{};      // ObjectReference.GetAngleX/Y()
		float hx{}, hy{};            // half the base bounds along local x and y (m)
	};

	// A reference the build placed (angles in radians, as the engine keeps them).
	struct Placed
	{
		std::uint32_t ref{ 0 }, base{ 0 };
		float x{}, y{}, z{}, ax{}, ay{}, az{};
	};

	// Where a misplaced reference should go (angles in degrees, for ObjectReference.SetAngle).
	struct Correction
	{
		std::uint32_t ref{ 0 };
		float x{}, y{}, z{}, ax{}, ay{}, az{};
	};

	// After a build: moves for the objects that did not land on their saved spot (all parts of a PackIn move
	// together, turned about the saved pivot), and every reference placed (positions as after those moves).
	std::vector<Correction> Corrections();
	std::vector<Placed> PlacedRefs();

	// Install the engine hooks (call once at plugin load, before the game runs).
	void Install();

	// Start building the jobs on a worker thread. The player must be in build mode at the outpost.
	// Returns false if a build is already running.
	bool Start(std::vector<Job> a_jobs);

	// Progress: [done, built, failed, total, running(0/1), aborted(0/1)]
	std::vector<std::int32_t> Progress();

	// Ask the worker to stop after the current object.
	void Stop();

	// True while build mode holds an item in placement.
	bool IsPlacing();

	// True if the build menu offers this form (directly or as a variant). The menu is filled in build mode.
	bool InBuildMenu(std::uint32_t a_formID);
}
