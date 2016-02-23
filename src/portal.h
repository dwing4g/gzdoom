#ifndef _PORTALS_H_
#define _PORTALS_H_

#include "basictypes.h"
#include "v_video.h"
#include "m_bbox.h"

struct FPortalGroupArray;
//============================================================================
//
// This table holds the offsets for the different parts of a map
// that are connected by portals.
// The idea here is basically the same as implemented in Eternity Engine:
//
// - each portal creates two sector groups in the map 
//   which are offset by the displacement of the portal anchors
//
// - for two or multiple groups the displacement is calculated by
//   adding the displacements between intermediate groups which
//   have to be traversed to connect the two
//
// - any sector not connected to any portal is assigned to group 0
//   Group 0 has no displacement to any other group in the level.
//
//============================================================================

struct FDisplacement
{
	fixedvec2 pos;
	bool isSet;
	BYTE indirect;	// just for illustration.

	operator fixedvec2()
	{
		return pos;
	}
};

struct FDisplacementTable
{
	TArray<FDisplacement> data;
	int size;

	FDisplacementTable()
	{
		Create(1);
	}

	void Create(int numgroups)
	{
		data.Resize(numgroups*numgroups);
		memset(&data[0], 0, numgroups*numgroups*sizeof(data[0]));
		size = numgroups;
	}

	FDisplacement &operator()(int x, int y)
	{
		if (x == y) return data[0];	// shortcut for the most common case
		return data[x + size*y];
	}
};

extern FDisplacementTable Displacements;

//============================================================================
//
// Flags and types for linedef portals
//
//============================================================================

enum
{
	PORTF_VISIBLE = 1,
	PORTF_PASSABLE = 2,
	PORTF_SOUNDTRAVERSE = 4,
	PORTF_INTERACTIVE = 8,

	PORTF_TYPETELEPORT = PORTF_VISIBLE | PORTF_PASSABLE | PORTF_SOUNDTRAVERSE,
	PORTF_TYPEINTERACTIVE = PORTF_VISIBLE | PORTF_PASSABLE | PORTF_SOUNDTRAVERSE | PORTF_INTERACTIVE,
};

enum
{
	PORTT_VISUAL,
	PORTT_TELEPORT,
	PORTT_INTERACTIVE,
	PORTT_LINKED,
	PORTT_LINKEDEE	// Eternity compatible definition which uses only one line ID and a different anchor type to link to.
};

enum
{
	PORG_ABSOLUTE,	// does not align at all. z-ccoordinates must match.
	PORG_FLOOR,
	PORG_CEILING,
};

enum
{
	PCOLL_NOTLINKED = 1,
	PCOLL_LINKED = 2
};

//============================================================================
//
// All information about a line-to-line portal (all types)
// There is no structure for sector plane portals because for historic
// reasons those use actors to connect.
//
//============================================================================

struct FLinePortal
{
	line_t *mOrigin;
	line_t *mDestination;
	fixed_t mXDisplacement;
	fixed_t mYDisplacement;
	BYTE mType;
	BYTE mFlags;
	BYTE mDefFlags;
	BYTE mAlign;
};

extern TArray<FLinePortal> linePortals;

void P_ClearPortals();
void P_SpawnLinePortal(line_t* line);
void P_FinalizePortals();
bool P_ChangePortal(line_t *ln, int thisid, int destid);
void P_CreateLinkedPortals();
bool P_CollectConnectedGroups(int startgroup, const fixedvec3 &position, fixed_t upperz, fixed_t checkradius, FPortalGroupArray &out);
void P_CollectLinkedPortals();
inline int P_NumPortalGroups()
{
	return Displacements.size;
}


/* code ported from prototype */
bool P_ClipLineToPortal(line_t* line, line_t* portal, fixed_t viewx, fixed_t viewy, bool partial = true, bool samebehind = true);
void P_TranslatePortalXY(line_t* src, line_t* dst, fixed_t& x, fixed_t& y);
void P_TranslatePortalVXVY(line_t* src, line_t* dst, fixed_t& vx, fixed_t& vy);
void P_TranslatePortalAngle(line_t* src, line_t* dst, angle_t& angle);
void P_TranslatePortalZ(line_t* src, line_t* dst, fixed_t& z);
void P_NormalizeVXVY(fixed_t& vx, fixed_t& vy);

//============================================================================
//
// basically, this is a teleporting tracer function,
// which can be used by itself (to calculate portal-aware offsets, say, for projectiles)
// or to teleport normal tracers (like hitscan, railgun, autoaim tracers)
//
//============================================================================

class PortalTracer
{
public:
	PortalTracer(fixed_t startx, fixed_t starty, fixed_t endx, fixed_t endy);

	// trace to next portal
	bool TraceStep();
	// trace to last available portal on the path
	void TraceAll() { while (TraceStep()) continue; }

	int depth;
	fixed_t startx;
	fixed_t starty;
	fixed_t intx;
	fixed_t inty;
	fixed_t intxIn;
	fixed_t intyIn;
	fixed_t endx;
	fixed_t endy;
	angle_t angle;
	fixed_t z;
	fixed_t frac;
	line_t* in;
	line_t* out;
	fixed_t vx;
	fixed_t vy;
};

/* new code */
fixed_t P_PointLineDistance(line_t* line, fixed_t x, fixed_t y);

#endif