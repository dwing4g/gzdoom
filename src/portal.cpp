/*
** portals.cpp
** Everything that has to do with portals (both of the line and sector variety)
**
**---------------------------------------------------------------------------
** Copyright 2016 ZZYZX
** Copyright 2016 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** There is no code here that is directly taken from Eternity
** although some similarities may be inevitable because it has to
** implement the same concepts.
*/


#include "p_local.h"
#include "p_blockmap.h"
#include "p_lnspec.h"
#include "r_bsp.h"
#include "r_segs.h"
#include "c_cvars.h"
#include "m_bbox.h"
#include "p_tags.h"
#include "farchive.h"
#include "v_text.h"
#include "a_sharedglobal.h"
#include "i_system.h"
#include "c_dispatch.h"
#include "p_maputl.h"
#include "p_spec.h"
#include "p_checkposition.h"

// simulation recurions maximum
CVAR(Int, sv_portal_recursions, 4, CVAR_ARCHIVE|CVAR_SERVERINFO)

FDisplacementTable Displacements;
FPortalBlockmap PortalBlockmap;

TArray<FLinePortal> linePortals;
TArray<FLinePortal*> linkedPortals;	// only the linked portals, this is used to speed up looking for them in P_CollectConnectedGroups.

//============================================================================
//
// This is used to mark processed portals for some collection functions.
//
//============================================================================

struct FPortalBits
{
	TArray<DWORD> data;

	void setSize(int num)
	{
		data.Resize((num + 31) / 32);
		clear();
	}

	void clear()
	{
		memset(&data[0], 0, data.Size()*sizeof(DWORD));
	}

	void setBit(int group)
	{
		data[group >> 5] |= (1 << (group & 31));
	}

	int getBit(int group)
	{
		return data[group >> 5] & (1 << (group & 31));
	}
};

//============================================================================
//
// BuildBlockmap
//
//============================================================================

static void BuildBlockmap()
{
	PortalBlockmap.Clear();
	PortalBlockmap.Create(bmapwidth, bmapheight);
	for (int y = 0; y < bmapheight; y++)
	{
		for (int x = 0; x < bmapwidth; x++)
		{
			int offset = y*bmapwidth + x;
			int *list = blockmaplump + *(blockmap + offset) + 1;
			FPortalBlock &block = PortalBlockmap(x, y);

			while (*list != -1)
			{
				line_t *ld = &lines[*list++];

				if (ld->isLinePortal())
				{
					PortalBlockmap.containsLines = true;
					block.portallines.Push(ld);
					block.neighborContainsLines = true;
					if (x > 0) PortalBlockmap(x - 1, y).neighborContainsLines = true;
					if (y > 0) PortalBlockmap(x, y - 1).neighborContainsLines = true;
					if (x < PortalBlockmap.dx - 1) PortalBlockmap(x + 1, y).neighborContainsLines = true;
					if (y < PortalBlockmap.dy - 1) PortalBlockmap(x, y + 1).neighborContainsLines = true;
				}
			}
		}
	}
	if (!PortalBlockmap.containsLines) PortalBlockmap.Clear();
}

//===========================================================================
//
// FLinePortalTraverse :: AddLineIntercepts.
//
// Similar to AddLineIntercepts but checks the portal blockmap for line-to-line portals
//
//===========================================================================

void FLinePortalTraverse::AddLineIntercepts(int bx, int by)
{
	FPortalBlock &block = PortalBlockmap(bx, by);

	for (unsigned i = 0; i<block.portallines.Size(); i++)
	{
		line_t *ld = block.portallines[i];
		fixed_t frac;
		divline_t dl;

		if (ld->validcount == validcount) continue;	// already processed

		if (P_PointOnDivlineSidePrecise (ld->v1->x, ld->v1->y, &trace) ==
			P_PointOnDivlineSidePrecise (ld->v2->x, ld->v2->y, &trace))
		{
			continue;		// line isn't crossed
		}
		P_MakeDivline (ld, &dl);
		if (P_PointOnDivlineSidePrecise (trace.x, trace.y, &dl) != 0 ||
			P_PointOnDivlineSidePrecise (trace.x+trace.dx, trace.y+trace.dy, &dl) != 1)
		{
			continue;		// line isn't crossed from the front side
		}

		// hit the line
		P_MakeDivline(ld, &dl);
		frac = P_InterceptVector(&trace, &dl);
		if (frac < 0 || frac > FRACUNIT) continue;	// behind source

		intercept_t newintercept;

		newintercept.frac = frac;
		newintercept.isaline = true;
		newintercept.done = false;
		newintercept.d.line = ld;
		intercepts.Push(newintercept);
	}
}

//============================================================================
//
// Save a line portal for savegames.
//
//============================================================================

FArchive &operator<< (FArchive &arc, FLinePortal &port)
{
	arc << port.mOrigin
		<< port.mDestination
		<< port.mXDisplacement
		<< port.mYDisplacement
		<< port.mType
		<< port.mFlags
		<< port.mDefFlags
		<< port.mAlign;
	return arc;
}


//============================================================================
//
// finds the destination for a line portal for spawning
//
//============================================================================

static line_t *FindDestination(line_t *src, int tag)
{
	if (tag)
	{
		int lineno = -1;
		FLineIdIterator it(tag);

		while ((lineno = it.Next()) >= 0)
		{
			if (&lines[lineno] != src)
			{
				return &lines[lineno];
			}
		}
	}
	return NULL;
}

//============================================================================
//
// Spawns a single line portal
//
//============================================================================

void P_SpawnLinePortal(line_t* line)
{
	// portal destination is special argument #0
	line_t* dst = NULL;

	if (line->args[2] >= PORTT_VISUAL && line->args[2] <= PORTT_LINKED)
	{
		dst = FindDestination(line, line->args[0]);

		line->portalindex = linePortals.Reserve(1);
		FLinePortal *port = &linePortals.Last();

		memset(port, 0, sizeof(FLinePortal));
		port->mOrigin = line;
		port->mDestination = dst;
		port->mType = BYTE(line->args[2]);	// range check is done above.

		if (port->mType == PORTT_LINKED)
		{
			// Linked portals have no z-offset ever.
			port->mAlign = PORG_ABSOLUTE;
		}
		else
		{
			port->mAlign = BYTE(line->args[3] >= PORG_ABSOLUTE && line->args[3] <= PORG_CEILING ? line->args[3] : PORG_ABSOLUTE);
			if (port->mType == PORTT_INTERACTIVE)
			{
				// Due to the way z is often handled, these pose a major issue for parts of the code that needs to transparently handle interactive portals.
				Printf(TEXTCOLOR_RED "Warning: z-offsetting not allowed for interactive portals. Changing line %d to teleport-portal!\n", int(line - lines));
				port->mType = PORTT_TELEPORT;
			}
		}
		if (port->mDestination != NULL)
		{
			port->mDefFlags = port->mType == PORTT_VISUAL ? PORTF_VISIBLE : port->mType == PORTT_TELEPORT ? PORTF_TYPETELEPORT : PORTF_TYPEINTERACTIVE;


		}
	}
	else if (line->args[2] == PORTT_LINKEDEE && line->args[0] == 0)
	{
		// EE-style portals require that the first line ID is identical and the first arg of the two linked linedefs are 0 and 1 respectively.

		int mytag = tagManager.GetFirstLineID(line);

		for (int i = 0; i < numlines; i++)
		{
			if (tagManager.GetFirstLineID(&lines[i]) == mytag && lines[i].args[0] == 1)
			{
				line->portalindex = linePortals.Reserve(1);
				FLinePortal *port = &linePortals.Last();

				memset(port, 0, sizeof(FLinePortal));
				port->mOrigin = line;
				port->mDestination = &lines[i];
				port->mType = PORTT_LINKED;
				port->mAlign = PORG_ABSOLUTE;
				port->mDefFlags = PORTF_TYPEINTERACTIVE;

				// we need to create the backlink here, too.
				lines[i].portalindex = linePortals.Reserve(1);
				port = &linePortals.Last();

				memset(port, 0, sizeof(FLinePortal));
				port->mOrigin = &lines[i];
				port->mDestination = line;
				port->mType = PORTT_LINKED;
				port->mAlign = PORG_ABSOLUTE;
				port->mDefFlags = PORTF_TYPEINTERACTIVE;

			}
		}
	}
	else
	{
		// undefined type
		return;
	}
}

//============================================================================
//
// Update a line portal's state after all have been spawned
//
//============================================================================

void P_UpdatePortal(FLinePortal *port)
{
	if (port->mDestination == NULL)
	{
		// Portal has no destination: switch it off
		port->mFlags = 0;
	}
	else if ((port->mOrigin->backsector == NULL && !(port->mOrigin->sidedef[0]->Flags & WALLF_POLYOBJ)) ||
		(port->mDestination->backsector == NULL && !(port->mOrigin->sidedef[0]->Flags & WALLF_POLYOBJ)))
	{
		// disable teleporting capability if a portal is or links to a one-sided wall (unless part of a polyobject.)
		port->mFlags = PORTF_VISIBLE;
	}
	else if (port->mDestination->getPortalDestination() != port->mOrigin)
	{
		//portal doesn't link back. This will be a simple teleporter portal.
		port->mFlags = port->mDefFlags & ~PORTF_INTERACTIVE;
		if (port->mType == PORTT_LINKED)
		{
			// this is illegal. Demote the type to TELEPORT
			port->mType = PORTT_TELEPORT;
			port->mDefFlags &= ~PORTF_INTERACTIVE;
		}
	}
	else
	{
		port->mFlags = port->mDefFlags;
		if (port->mType == PORTT_LINKED)
		{
			if (linePortals[port->mDestination->portalindex].mType != PORTT_LINKED)
			{
				port->mType = PORTT_INTERACTIVE;	// linked portals must be two-way.
			}
			else
			{
				port->mXDisplacement = port->mDestination->v2->x - port->mOrigin->v1->x;
				port->mYDisplacement = port->mDestination->v2->y - port->mOrigin->v1->y;
			}
		}
 	}
}

//============================================================================
//
// Collect a separate list of linked portals so that these can be
// processed faster without the simpler types interfering.
//
//============================================================================

void P_CollectLinkedPortals()
{
	linkedPortals.Clear();
	for (unsigned i = 0; i < linePortals.Size(); i++)
	{
		FLinePortal * port = &linePortals[i];
		if (port->mType == PORTT_LINKED)
		{
			linkedPortals.Push(port);
		}
	}
}

//============================================================================
//
// Post-process all line portals
//
//============================================================================

void P_FinalizePortals()
{
	for (unsigned i = 0; i < linePortals.Size(); i++)
	{
		FLinePortal * port = &linePortals[i];
		P_UpdatePortal(port);
	}
	P_CollectLinkedPortals();
	BuildBlockmap();
	P_CreateLinkedPortals();
}

//============================================================================
//
// Change the destination of a portal
//
//============================================================================

static bool ChangePortalLine(line_t *line, int destid)
{
	if (line->portalindex >= linePortals.Size()) return false;
	FLinePortal *port = &linePortals[line->portalindex];
	if (port->mType == PORTT_LINKED) return false;	// linked portals cannot be changed.
	if (destid == 0) port->mDestination = NULL;
	port->mDestination = FindDestination(line, destid);
	if (port->mDestination == NULL)
	{
		port->mFlags = 0;
	}
	else if (port->mType == PORTT_INTERACTIVE)
	{
		FLinePortal *portd = &linePortals[port->mDestination->portalindex];
		if (portd != NULL && portd->mType == PORTT_INTERACTIVE && portd->mDestination == line)
		{
			// this is a 2-way interactive portal
			port->mFlags = port->mDefFlags | PORTF_INTERACTIVE;
			portd->mFlags = portd->mDefFlags | PORTF_INTERACTIVE;
		}
		else
		{
			port->mFlags = port->mDefFlags;
			portd->mFlags = portd->mDefFlags;
		}
	}
	return true;
}


//============================================================================
//
// Change the destination of a group of portals
//
//============================================================================

bool P_ChangePortal(line_t *ln, int thisid, int destid)
{
	int lineno;

	if (thisid == 0) return ChangePortalLine(ln, destid);
	FLineIdIterator it(thisid);
	bool res = false;
	while ((lineno = it.Next()) >= 0)
	{
		res |= ChangePortalLine(&lines[lineno], destid);
	}
	return res;
}

//============================================================================
//
// clears all portal dat for a new level start
//
//============================================================================

void P_ClearPortals()
{
	Displacements.Create(1);
	linePortals.Clear();
	linkedPortals.Clear();
}


//============================================================================
//
// Calculate the intersection between two lines.
// [ZZ] lots of floats here to avoid overflowing a lot
//
//============================================================================

bool P_IntersectLines(fixed_t o1x, fixed_t o1y, fixed_t p1x, fixed_t p1y,
				      fixed_t o2x, fixed_t o2y, fixed_t p2x, fixed_t p2y,
				      fixed_t& rx, fixed_t& ry)
{
	double xx = FIXED2DBL(o2x) - FIXED2DBL(o1x);
	double xy = FIXED2DBL(o2y) - FIXED2DBL(o1y);

	double d1x = FIXED2DBL(p1x) - FIXED2DBL(o1x);
	double d1y = FIXED2DBL(p1y) - FIXED2DBL(o1y);

	if (d1x > d1y)
	{
		d1y = d1y / d1x * 32767.0f;
		d1x = 32767.0;
	}
	else
	{
		d1x = d1x / d1y * 32767.0f;
		d1y = 32767.0;
	}

	double d2x = FIXED2DBL(p2x) - FIXED2DBL(o2x);
	double d2y = FIXED2DBL(p2y) - FIXED2DBL(o2y);

	double cross = d1x*d2y - d1y*d2x;
	if (fabs(cross) < 1e-8)
		return false;

	double t1 = (xx * d2y - xy * d2x)/cross;
	rx = o1x + FLOAT2FIXED(d1x * t1);
	ry = o1y + FLOAT2FIXED(d1y * t1);
	return true;
}

inline int P_PointOnLineSideExplicit (fixed_t x, fixed_t y, fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
	return DMulScale32 (y-y1, x2-x1, x1-x, y2-y1) > 0;
}

//============================================================================
//
// check if this line is between portal and the viewer. clip away if it is.
// (this may need some fixing)
//
//============================================================================

bool P_ClipLineToPortal(line_t* line, line_t* portal, fixed_t viewx, fixed_t viewy, bool partial, bool samebehind)
{
	bool behind1 = !!P_PointOnLineSidePrecise(line->v1->x, line->v1->y, portal);
	bool behind2 = !!P_PointOnLineSidePrecise(line->v2->x, line->v2->y, portal);

	// [ZZ] update 16.12.2014: if a vertex equals to one of portal's vertices, it's treated as being behind the portal.
	//                         this is required in order to clip away diagonal lines around the portal (example: 1-sided triangle shape with a mirror on it's side)
	if ((line->v1->x == portal->v1->x && line->v1->y == portal->v1->y) ||
		(line->v1->x == portal->v2->x && line->v1->y == portal->v2->y))
			behind1 = samebehind;
	if ((line->v2->x == portal->v1->x && line->v2->y == portal->v1->y) ||
		(line->v2->x == portal->v2->x && line->v2->y == portal->v2->y))
			behind2 = samebehind;

	if (behind1 && behind2)
	{
		// line is behind the portal plane. now check if it's in front of two view plane borders (i.e. if it will get in the way of rendering)
		fixed_t dummyx, dummyy;
		bool infront1 = P_IntersectLines(line->v1->x, line->v1->y, line->v2->x, line->v2->y, viewx, viewy, portal->v1->x, portal->v1->y, dummyx, dummyy);
		bool infront2 = P_IntersectLines(line->v1->x, line->v1->y, line->v2->x, line->v2->y, viewx, viewy, portal->v2->x, portal->v2->y, dummyx, dummyy);
		if (infront1 && infront2)
			return true;
	}

	return false;
}

//============================================================================
//
// Translates a coordinate by a portal's displacement
//
//============================================================================

void P_TranslatePortalXY(line_t* src, line_t* dst, fixed_t& x, fixed_t& y)
{
	if (!src || !dst)
		return;

	fixed_t nposx, nposy;	// offsets from line

	// Get the angle between the two linedefs, for rotating
	// orientation and velocity. Rotate 180 degrees, and flip
	// the position across the exit linedef, if reversed.
	angle_t angle =
			R_PointToAngle2(0, 0, dst->dx, dst->dy) -
			R_PointToAngle2(0, 0, src->dx, src->dy);

	angle += ANGLE_180;

	// Sine, cosine of angle adjustment
	fixed_t s = finesine[angle>>ANGLETOFINESHIFT];
	fixed_t c = finecosine[angle>>ANGLETOFINESHIFT];

	fixed_t tx, ty;

	nposx = x - src->v1->x;
	nposy = y - src->v1->y;

	// Rotate position along normal to match exit linedef
	tx = FixedMul(nposx, c) - FixedMul(nposy, s);
	ty = FixedMul(nposy, c) + FixedMul(nposx, s);

	tx += dst->v2->x;
	ty += dst->v2->y;

	x = tx;
	y = ty;
}

//============================================================================
//
// Translates a velocity vector by a portal's displacement
//
//============================================================================

void P_TranslatePortalVXVY(line_t* src, line_t* dst, fixed_t& vx, fixed_t& vy)
{
	angle_t angle =
		R_PointToAngle2(0, 0, dst->dx, dst->dy) -
		R_PointToAngle2(0, 0, src->dx, src->dy);

	angle += ANGLE_180;

	// Sine, cosine of angle adjustment
	fixed_t s = finesine[angle>>ANGLETOFINESHIFT];
	fixed_t c = finecosine[angle>>ANGLETOFINESHIFT];

	fixed_t orig_velx = vx;
	fixed_t orig_vely = vy;
	vx = FixedMul(orig_velx, c) - FixedMul(orig_vely, s);
	vy = FixedMul(orig_vely, c) + FixedMul(orig_velx, s);
}

//============================================================================
//
// Translates an angle by a portal's displacement
//
//============================================================================

void P_TranslatePortalAngle(line_t* src, line_t* dst, angle_t& angle)
{
	if (!src || !dst)
		return;

	// Get the angle between the two linedefs, for rotating
	// orientation and velocity. Rotate 180 degrees, and flip
	// the position across the exit linedef, if reversed.
	angle_t xangle =
			R_PointToAngle2(0, 0, dst->dx, dst->dy) -
			R_PointToAngle2(0, 0, src->dx, src->dy);

	xangle += ANGLE_180;
	angle += xangle;
}

//============================================================================
//
// Translates a z-coordinate by a portal's displacement
//
//============================================================================

void P_TranslatePortalZ(line_t* src, line_t* dst, fixed_t& z)
{
	// args[2] = 0 - no adjustment
	// args[2] = 1 - adjust by floor difference
	// args[2] = 2 - adjust by ceiling difference

	switch (src->getPortalAlignment())
	{
	case PORG_FLOOR:
		z = z - src->frontsector->floorplane.ZatPoint(src->v1->x, src->v1->y) + dst->frontsector->floorplane.ZatPoint(dst->v2->x, dst->v2->y);
		return;

	case PORG_CEILING:
		z = z - src->frontsector->ceilingplane.ZatPoint(src->v1->x, src->v1->y) + dst->frontsector->ceilingplane.ZatPoint(dst->v2->x, dst->v2->y);
		return;

	default:
		return;
	}
}

//============================================================================
//
// calculate shortest distance from a point (x,y) to a linedef
//
//============================================================================

fixed_t P_PointLineDistance(line_t* line, fixed_t x, fixed_t y)
{
	angle_t angle = R_PointToAngle2(0, 0, line->dx, line->dy);
	angle += ANGLE_180;

	fixed_t dx = line->v1->x - x;
	fixed_t dy = line->v1->y - y;

	fixed_t s = finesine[angle>>ANGLETOFINESHIFT];
	fixed_t c = finecosine[angle>>ANGLETOFINESHIFT];

	fixed_t d2x = FixedMul(dx, c) - FixedMul(dy, s);

	return abs(d2x);
}

void P_NormalizeVXVY(fixed_t& vx, fixed_t& vy)
{
	double _vx = FIXED2DBL(vx);
	double _vy = FIXED2DBL(vy);
	double len = sqrt(_vx*_vx+_vy*_vy);
	vx = FLOAT2FIXED(_vx/len);
	vy = FLOAT2FIXED(_vy/len);
}

//============================================================================
//
// P_GetOffsetPosition
//
// Offsets a given coordinate if the trace from the origin crosses an 
// interactive line-to-line portal.
//
//============================================================================

fixedvec2 P_GetOffsetPosition(AActor *actor, fixed_t dx, fixed_t dy)
{
	fixedvec2 dest = { actor->X() + dx, actor->Y() + dy };
	if (PortalBlockmap.containsLines)
	{
		fixed_t actx = actor->X(), acty = actor->Y();
		// Try some easily discoverable early-out first. If we know that the trace cannot possibly find a portal, this saves us from calling the traverser completely for vast parts of the map.
		if (dx < 128 * FRACUNIT && dy < 128 * FRACUNIT)
		{
			fixed_t blockx = GetSafeBlockX(actx - bmaporgx);
			fixed_t blocky = GetSafeBlockX(acty - bmaporgy);
			if (blockx < 0 || blocky < 0 || blockx >= bmapwidth || blocky >= bmapheight || !PortalBlockmap(blockx, blocky).neighborContainsLines) return dest;
		}

		FLinePortalTraverse it;
		bool repeat;
		do
		{
			it.init(actx, acty, dx, dy, PT_ADDLINES|PT_DELTA);
			intercept_t *in;

			repeat = false;
			while ((in = it.Next()))
			{
				// hit a portal line.
				line_t *line = in->d.line;
				FLinePortal *port = line->getPortal();
				line_t* out = port->mDestination;

				// Teleport portals are intentionally ignored since skipping this stuff is their entire reason for existence.
				if (port->mFlags & PORTF_INTERACTIVE)
				{
					fixed_t hitdx = FixedMul(it.Trace().dx, in->frac);
					fixed_t hitdy = FixedMul(it.Trace().dy, in->frac);

					if (port->mType == PORTT_LINKED)
					{
						// optimized handling for linked portals where we only need to add an offset.
						actx = it.Trace().x + hitdx + port->mXDisplacement;
						acty = it.Trace().y + hitdy + port->mYDisplacement;
						dest.x += port->mXDisplacement;
						dest.y += port->mYDisplacement;
					}
					else
					{
						// interactive ones are more complex because the vector may be rotated.
						// Note: There is no z-translation here, there's just too much code in the engine that wouldn't be able to handle interactive portals with a height difference.
						actx = it.Trace().x + hitdx;
						acty = it.Trace().y + hitdy;

						P_TranslatePortalXY(line, out, actx, acty);
						P_TranslatePortalXY(line, out, dest.x, dest.y);
					}
					// update the fields, end this trace and restart from the new position
					dx = dest.x - actx;
					dy = dest.y - acty;
					repeat = true;
				}

				break;
			}
		} while (repeat);
	}
	return dest;
}


//============================================================================
//
// CollectSectors
//
// Collects all sectors that are connected to any sector belonging to a portal
// because they all will need the same displacement values
//
//============================================================================

static bool CollectSectors(int groupid, sector_t *origin)
{
	if (origin->PortalGroup != 0) return false;	// already processed
	origin->PortalGroup = groupid;

	TArray<sector_t *> list(16);
	list.Push(origin);

	for (unsigned i = 0; i < list.Size(); i++)
	{
		sector_t *sec = list[i];

		for (int j = 0; j < sec->linecount; j++)
		{
			line_t *line = sec->lines[j];
			sector_t *other = line->frontsector == sec ? line->backsector : line->frontsector;
			if (other != NULL && other != sec && other->PortalGroup != groupid)
			{
				other->PortalGroup = groupid;
				list.Push(other);
			}
		}
	}
	return true;
}


//============================================================================
//
// AddDisplacementForPortal
//
// Adds the displacement for one portal to the displacement array
// (one version for sector to sector plane, one for line to line portals)
//
// Note: Despite the similarities to Eternity's equivalent this is
// original code!
//
//============================================================================

static void AddDisplacementForPortal(AStackPoint *portal)
{
	int thisgroup = portal->Mate->Sector->PortalGroup;
	int othergroup = portal->Sector->PortalGroup;
	if (thisgroup == othergroup)
	{
		Printf("Portal between sectors %d and %d has both sides in same group and will be disabled\n", portal->Sector->sectornum, portal->Mate->Sector->sectornum);
		portal->special1 = portal->Mate->special1 = SKYBOX_PORTAL;
		return;
	}
	if (thisgroup <= 0 || thisgroup >= Displacements.size || othergroup <= 0 || othergroup >= Displacements.size)
	{
		Printf("Portal between sectors %d and %d has invalid group and will be disabled\n", portal->Sector->sectornum, portal->Mate->Sector->sectornum);
		portal->special1 = portal->Mate->special1 = SKYBOX_PORTAL;
		return;
	}

	FDisplacement & disp = Displacements(thisgroup, othergroup);
	if (!disp.isSet)
	{
		disp.pos.x = portal->scaleX;
		disp.pos.y = portal->scaleY;
		disp.isSet = true;
	}
	else
	{
		if (disp.pos.x != portal->scaleX || disp.pos.y != portal->scaleY)
		{
			Printf("Portal between sectors %d and %d has displacement mismatch and will be disabled\n", portal->Sector->sectornum, portal->Mate->Sector->sectornum);
			portal->special1 = portal->Mate->special1 = SKYBOX_PORTAL;
			return;
		}
	}
}


static void AddDisplacementForPortal(FLinePortal *portal)
{
	int thisgroup = portal->mOrigin->frontsector->PortalGroup;
	int othergroup = portal->mDestination->frontsector->PortalGroup;
	if (thisgroup == othergroup)
	{
		Printf("Portal between lines %d and %d has both sides in same group\n", int(portal->mOrigin-lines), int(portal->mDestination-lines));
		portal->mType = linePortals[portal->mDestination->portalindex].mType = PORTT_TELEPORT;
		return;
	}
	if (thisgroup <= 0 || thisgroup >= Displacements.size || othergroup <= 0 || othergroup >= Displacements.size)
	{
		Printf("Portal between lines %d and %d has invalid group\n", int(portal->mOrigin - lines), int(portal->mDestination - lines));
		portal->mType = linePortals[portal->mDestination->portalindex].mType = PORTT_TELEPORT;
		return;
	}

	FDisplacement & disp = Displacements(thisgroup, othergroup);
	if (!disp.isSet)
	{
		disp.pos.x = portal->mXDisplacement;
		disp.pos.y = portal->mYDisplacement;
		disp.isSet = true;
	}
	else
	{
		if (disp.pos.x != portal->mXDisplacement || disp.pos.y != portal->mYDisplacement)
		{
			Printf("Portal between lines %d and %d has displacement mismatch\n", int(portal->mOrigin - lines), int(portal->mDestination - lines));
			portal->mType = linePortals[portal->mDestination->portalindex].mType = PORTT_TELEPORT;
			return;
		}
	}
}

//============================================================================
//
// ConnectGroups
//
// Do the indirect connections. This loop will run until it cannot find any new connections
//
//============================================================================

static bool ConnectGroups()
{
	// Now 
	BYTE indirect = 1;
	bool bogus = false;
	bool changed;
	do
	{
		changed = false;
		for (int x = 1; x < Displacements.size; x++)
		{
			for (int y = 1; y < Displacements.size; y++)
			{
				FDisplacement &dispxy = Displacements(x, y);
				if (dispxy.isSet)
				{
					for (int z = 1; z < Displacements.size; z++)
					{
						FDisplacement &dispyz = Displacements(y, z);
						if (dispyz.isSet)
						{
							FDisplacement &dispxz = Displacements(x, z);
							if (dispxz.isSet)
							{
								if (dispxy.pos.x + dispyz.pos.x != dispxz.pos.x || dispxy.pos.y + dispyz.pos.y != dispxz.pos.y)
								{
									bogus = true;
								}
							}
							else
							{
								dispxz.pos = dispxy.pos + dispyz.pos;
								dispxz.isSet = true;
								dispxz.indirect = indirect;
								changed = true;
							}
						}
					}
				}
			}
		}
		indirect++;
	} while (changed);
	return bogus;
}


//============================================================================
//
// P_CreateLinkedPortals
//
// Creates the data structures needed for linked portals
// Removes portals from sloped sectors (as they cannot work on them)
// Group all sectors connected to one side of the portal
// Caclculate displacements between all created groups.
//
// Portals with the same offset but different anchors will not be merged.
//
//============================================================================

void P_CreateLinkedPortals()
{
	TThinkerIterator<AStackPoint> it;
	AStackPoint *mo;
	TArray<AStackPoint *> orgs;
	int id = 0;
	bool bogus = false;

	while ((mo = it.Next()))
	{
		if (mo->special1 == SKYBOX_LINKEDPORTAL)
		{
			if (mo->Mate != NULL)
			{
				orgs.Push(mo);
				mo->reactiontime = ++id;
			}
			else
			{
				// this should never happen, but if it does, the portal needs to be removed
				mo->Destroy();
			}
		}
	}
	if (orgs.Size() == 0)
	{
		// Create the 0->0 translation which is always needed.
		Displacements.Create(1);
		return;
	}
	for (int i = 0; i < numsectors; i++)
	{
		for (int j = 0; j < 2; j++)
		{
			AActor *box = sectors[i].SkyBoxes[j];
			if (box != NULL && box->special1 == SKYBOX_LINKEDPORTAL)
			{
				secplane_t &plane = j == 0 ? sectors[i].floorplane : sectors[i].ceilingplane;
				if (plane.a || plane.b)
				{
					// The engine cannot deal with portals on a sloped plane.
					sectors[i].SkyBoxes[j] = NULL;
					Printf("Portal on %s of sector %d is sloped and will be disabled\n", j==0? "floor":"ceiling", i);
				}
			}
		}
	}

	// Group all sectors, starting at each portal origin.
	id = 1;
	for (unsigned i = 0; i < orgs.Size(); i++)
	{
		if (CollectSectors(id, orgs[i]->Sector)) id++;
		if (CollectSectors(id, orgs[i]->Mate->Sector)) id++;
	}
	for (unsigned i = 0; i < linePortals.Size(); i++)
	{
		if (linePortals[i].mType == PORTT_LINKED)
		{
			if (CollectSectors(id, linePortals[i].mOrigin->frontsector)) id++;
			if (CollectSectors(id, linePortals[i].mDestination->frontsector)) id++;
		}
	}

	Displacements.Create(id);
	// Check for leftover sectors that connect to a portal
	for (int i = 0; i<numsectors; i++)
	{
		for (int j = 0; j < 2; j++)
		{
			ASkyViewpoint *box = barrier_cast<ASkyViewpoint*>(sectors[i].SkyBoxes[j]);
			if (box != NULL)
			{
				if (box->special1 == SKYBOX_LINKEDPORTAL && sectors[i].PortalGroup == 0)
				{
					// Note: the linked actor will be on the other side of the portal.
					// To get this side's group we will have to look at the mate object.
					CollectSectors(box->Mate->Sector->PortalGroup, &sectors[i]);
					// We cannot process the backlink here because all we can access is the anchor object
					// If necessary that will have to be done for the other side's portal.
				}
			}
		}
	}
	for (unsigned i = 0; i < orgs.Size(); i++)
	{
		AddDisplacementForPortal(orgs[i]);
	}
	for (unsigned i = 0; i < linePortals.Size(); i++)
	{
		if (linePortals[i].mType == PORTT_LINKED)
		{
			AddDisplacementForPortal(&linePortals[i]);
		}
	}

	for (int x = 1; x < Displacements.size; x++)
	{
		for (int y = x + 1; y < Displacements.size; y++)
		{
			FDisplacement &dispxy = Displacements(x, y);
			FDisplacement &dispyx = Displacements(y, x);
			if (dispxy.isSet && dispyx.isSet &&
				(dispxy.pos.x != -dispyx.pos.x || dispxy.pos.y != -dispyx.pos.y))
			{
				int sec1 = -1, sec2 = -1;
				for (int i = 0; i < numsectors && (sec1 == -1 || sec2 == -1); i++)
				{
					if (sec1 == -1 && sectors[i].PortalGroup == x)  sec1 = i;
					if (sec2 == -1 && sectors[i].PortalGroup == y)  sec2 = i;
				}
				Printf("Link offset mismatch between sectors %d and %d\n", sec1, sec2);
				bogus = true;
			}
			// mark everything that connects to a one-sided line
			for (int i = 0; i < numlines; i++)
			{
				if (lines[i].backsector == NULL && lines[i].frontsector->PortalGroup == 0)
				{
					CollectSectors(-1, lines[i].frontsector);
				}
			}
			// and now print a message for everything that still wasn't processed.
			for (int i = 0; i < numsectors; i++)
			{
				if (sectors[i].PortalGroup == 0)
				{
					Printf("Unable to assign sector %d to any group. Possibly self-referencing\n", i);
				}
				else if (sectors[i].PortalGroup == -1) sectors[i].PortalGroup = 0;
			}
		}
	}
	bogus |= ConnectGroups();
	if (bogus)
	{
		// todo: disable all portals whose offsets do not match the associated groups
	}

	// reject would just get in the way when checking sight through portals.
	if (rejectmatrix != NULL)
	{
		delete[] rejectmatrix;
		rejectmatrix = NULL;
	}
	// finally we must flag all planes which are obstructed by the sector's own ceiling or floor.
	for (int i = 0; i < numsectors; i++)
	{
		sectors[i].CheckPortalPlane(sector_t::floor);
		sectors[i].CheckPortalPlane(sector_t::ceiling);
	}
	//BuildBlockmap();
}


//============================================================================
//
// Collect all portal groups this actor would occupy at the given position
// This is used to determine which parts of the map need to be checked.
//
//============================================================================

bool P_CollectConnectedGroups(int startgroup, const fixedvec3 &position, fixed_t upperz, fixed_t checkradius, FPortalGroupArray &out)
{
	// Keep this temporary work stuff static. This function can never be called recursively
	// and this would have to be reallocated for each call otherwise.
	static FPortalBits processMask;
	static TArray<FLinePortal*> foundPortals;

	bool retval = false;
	out.inited = true;
	if (linkedPortals.Size() == 0)
	{
		// If there are no portals, all sectors are in group 0.
		return false;
	}
	processMask.setSize(linkedPortals.Size());
	processMask.clear();
	foundPortals.Clear();

	int thisgroup = startgroup;
	processMask.setBit(thisgroup);
	//out.Add(thisgroup);

	for (unsigned i = 0; i < linkedPortals.Size(); i++)
	{
		line_t *ld = linkedPortals[i]->mOrigin;
		int othergroup = ld->frontsector->PortalGroup;
		FDisplacement &disp = Displacements(thisgroup, othergroup);
		if (!disp.isSet) continue;	// no connection.

		FBoundingBox box(position.x + disp.pos.x, position.y + disp.pos.y, checkradius);

		if (box.Right() <= ld->bbox[BOXLEFT]
			|| box.Left() >= ld->bbox[BOXRIGHT]
			|| box.Top() <= ld->bbox[BOXBOTTOM]
			|| box.Bottom() >= ld->bbox[BOXTOP])
			continue;	// not touched

		if (box.BoxOnLineSide(linkedPortals[i]->mOrigin) != -1) continue;	// not touched
		foundPortals.Push(linkedPortals[i]);
	}
	bool foundone = true;
	while (foundone)
	{
		foundone = false;
		for (int i = foundPortals.Size() - 1; i >= 0; i--)
		{
			if (processMask.getBit(foundPortals[i]->mOrigin->frontsector->PortalGroup) && 
				!processMask.getBit(foundPortals[i]->mDestination->frontsector->PortalGroup))
			{
				processMask.setBit(foundPortals[i]->mDestination->frontsector->PortalGroup);
				out.Add(foundPortals[i]->mDestination->frontsector->PortalGroup);
				foundone = true;
				retval = true;
				foundPortals.Delete(i);
			}
		}
	}
	sector_t *sec = P_PointInSector(position.x, position.y);
	sector_t *wsec = sec;
	while (!wsec->PortalBlocksMovement(sector_t::ceiling) && upperz > wsec->SkyBoxes[sector_t::ceiling]->threshold)
	{
		sector_t *othersec = wsec->SkyBoxes[sector_t::ceiling]->Sector;
		fixedvec2 pos = Displacements.getOffset(startgroup, othersec->PortalGroup);
		fixed_t dx = position.x + pos.x;
		fixed_t dy = position.y + pos.y;
		processMask.setBit(othersec->PortalGroup);
		out.Add(othersec->PortalGroup|FPortalGroupArray::UPPER);
		wsec = P_PointInSector(dx, dy);	// get upper sector at the exact spot we want to check and repeat
		retval = true;
	}
	wsec = sec;
	while (!wsec->PortalBlocksMovement(sector_t::floor) && position.z < wsec->SkyBoxes[sector_t::floor]->threshold)
	{
		sector_t *othersec = wsec->SkyBoxes[sector_t::floor]->Sector;
		fixedvec2 pos = Displacements.getOffset(startgroup, othersec->PortalGroup);
		fixed_t dx = position.x + pos.x;
		fixed_t dy = position.y + pos.y;
		processMask.setBit(othersec->PortalGroup|FPortalGroupArray::LOWER);
		out.Add(othersec->PortalGroup);
		wsec = P_PointInSector(dx, dy);	// get lower sector at the exact spot we want to check and repeat
		retval = true;
	}
	return retval;
}


//============================================================================
//
// print the group link table to the console
//
//============================================================================

CCMD(dumplinktable)
{
	for (int x = 1; x < Displacements.size; x++)
	{
		for (int y = 1; y < Displacements.size; y++)
		{
			FDisplacement &disp = Displacements(x, y);
			Printf("%c%c(%6d, %6d)", TEXTCOLOR_ESCAPE, 'C' + disp.indirect, disp.pos.x >> FRACBITS, disp.pos.y >> FRACBITS);
		}
		Printf("\n");
	}
}




