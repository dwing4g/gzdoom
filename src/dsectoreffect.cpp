// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//		Base class for effects on sectors.
//		[RH] Created this class hierarchy.
//
//-----------------------------------------------------------------------------

#include "dsectoreffect.h"
#include "gi.h"
#include "p_local.h"
#include "p_3dmidtex.h"
#include "r_data/r_interpolate.h"
#include "statnums.h"
#include "farchive.h"
#include "doomstat.h"

IMPLEMENT_CLASS (DSectorEffect)

DSectorEffect::DSectorEffect ()
: DThinker(STAT_SECTOREFFECT)
{
	m_Sector = NULL;
}

void DSectorEffect::Destroy()
{
	if (m_Sector)
	{
		if (m_Sector->floordata == this)
		{
			m_Sector->floordata = NULL;
		}
		if (m_Sector->ceilingdata == this)
		{
			m_Sector->ceilingdata = NULL;
		}
		if (m_Sector->lightingdata == this)
		{
			m_Sector->lightingdata = NULL;
		}
	}
	Super::Destroy();
}

DSectorEffect::DSectorEffect (sector_t *sector)
{
	m_Sector = sector;
}

void DSectorEffect::Serialize (FArchive &arc)
{
	Super::Serialize (arc);
	arc << m_Sector;
}

IMPLEMENT_POINTY_CLASS (DMover)
	DECLARE_POINTER(interpolation)
END_POINTERS

DMover::DMover ()
{
}

DMover::DMover (sector_t *sector)
	: DSectorEffect (sector)
{
	interpolation = NULL;
}

void DMover::Destroy()
{
	StopInterpolation();
	Super::Destroy();
}

void DMover::Serialize (FArchive &arc)
{
	Super::Serialize (arc);
	arc << interpolation;
}

void DMover::StopInterpolation(bool force)
{
	if (interpolation != NULL)
	{
		interpolation->DelRef(force);
		interpolation = NULL;
	}
}



IMPLEMENT_CLASS (DMovingFloor)

DMovingFloor::DMovingFloor ()
{
}

DMovingFloor::DMovingFloor (sector_t *sector)
	: DMover (sector)
{
	sector->floordata = this;
	interpolation = sector->SetInterpolation(sector_t::FloorMove, true);
}

IMPLEMENT_CLASS (DMovingCeiling)

DMovingCeiling::DMovingCeiling ()
{
}

DMovingCeiling::DMovingCeiling (sector_t *sector)
	: DMover (sector)
{
	sector->ceilingdata = this;
	interpolation = sector->SetInterpolation(sector_t::CeilingMove, true);
}

bool DMover::MoveAttached(int crush, double move, int floorOrCeiling, bool resetfailed)
{
	if (!P_Scroll3dMidtex(m_Sector, crush, move, !!floorOrCeiling) && resetfailed)
	{
		P_Scroll3dMidtex(m_Sector, crush, -move, !!floorOrCeiling);
		return false;
	}
	if (!P_MoveLinkedSectors(m_Sector, crush, move, !!floorOrCeiling) && resetfailed)
	{
		P_MoveLinkedSectors(m_Sector, crush, -move, !!floorOrCeiling);
		P_Scroll3dMidtex(m_Sector, crush, -move, !!floorOrCeiling);
		return false;
	}
	return true;
}

//
// Move a plane (floor or ceiling) and check for crushing
// [RH] Crush specifies the actual amount of crushing damage inflictable.
//		(Use -1 to prevent it from trying to crush)
//		dest is the desired d value for the plane
//
DMover::EResult DMover::MovePlane (double speed, double dest, int crush,
								   int floorOrCeiling, int direction, bool hexencrush)
{
	bool	 	flag;
	double 	lastpos;
	double		movedest;
	double		move;
	//double		destheight;	//jff 02/04/98 used to keep floors/ceilings
							// from moving thru each other
	switch (floorOrCeiling)
	{
	case 0:
		// FLOOR
		lastpos = m_Sector->floorplane.fD();
		switch (direction)
		{
		case -1:
			// DOWN
			movedest = m_Sector->floorplane.GetChangedHeight (-speed);
			if (movedest >= dest)
			{
				move = m_Sector->floorplane.HeightDiff (lastpos, dest);

				if (!MoveAttached(crush, move, 0, true)) return crushed;

				m_Sector->floorplane.setD(dest);
				flag = P_ChangeSector (m_Sector, crush, move, 0, false);
				if (flag)
				{
					m_Sector->floorplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, -move, 0, true);
					MoveAttached(crush, -move, 0, false);
				}
				else
				{
					m_Sector->ChangePlaneTexZ(sector_t::floor, move);
					m_Sector->AdjustFloorClip ();
				}
				return pastdest;
			}
			else
			{
				if (!MoveAttached(crush, -speed, 0, true)) return crushed;

				m_Sector->floorplane.setD(movedest);

				flag = P_ChangeSector (m_Sector, crush, -speed, 0, false);
				if (flag)
				{
					m_Sector->floorplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, speed, 0, true);
					MoveAttached(crush, speed, 0, false);
					return crushed;
				}
				else
				{
					m_Sector->ChangePlaneTexZ(sector_t::floor, m_Sector->floorplane.HeightDiff (lastpos));
					m_Sector->AdjustFloorClip ();
				}
			}
			break;
												
		case 1:
			// UP
			// jff 02/04/98 keep floor from moving thru ceilings
			// [RH] not so easy with arbitrary planes
			//destheight = (dest < m_Sector->ceilingheight) ? dest : m_Sector->ceilingheight;
			if (!m_Sector->ceilingplane.isSlope() && !m_Sector->floorplane.isSlope() &&
				(!(i_compatflags2 & COMPATF2_FLOORMOVE) && -dest > m_Sector->ceilingplane.fD()))
			{
				dest = -m_Sector->ceilingplane.fD();
			}

			movedest = m_Sector->floorplane.GetChangedHeight (speed);

			if (movedest <= dest)
			{
				move = m_Sector->floorplane.HeightDiff (lastpos, dest);

				if (!MoveAttached(crush, move, 0, true)) return crushed;

				m_Sector->floorplane.setD(dest);

				flag = P_ChangeSector (m_Sector, crush, move, 0, false);
				if (flag)
				{
					m_Sector->floorplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, -move, 0, true);
					MoveAttached(crush, -move, 0, false);
				}
				else
				{
					m_Sector->ChangePlaneTexZ(sector_t::floor, move);
					m_Sector->AdjustFloorClip ();
				}
				return pastdest;
			}
			else
			{
				if (!MoveAttached(crush, speed, 0, true)) return crushed;

				m_Sector->floorplane.setD(movedest);

				// COULD GET CRUSHED
				flag = P_ChangeSector (m_Sector, crush, speed, 0, false);
				if (flag)
				{
					if (crush >= 0 && !hexencrush)
					{
						m_Sector->ChangePlaneTexZ(sector_t::floor, m_Sector->floorplane.HeightDiff (lastpos));
						m_Sector->AdjustFloorClip ();
						return crushed;
					}
					m_Sector->floorplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, -speed, 0, true);
					MoveAttached(crush, -speed, 0, false);
					return crushed;
				}
				m_Sector->ChangePlaneTexZ(sector_t::floor, m_Sector->floorplane.HeightDiff (lastpos));
				m_Sector->AdjustFloorClip ();
			}
			break;
		}
		break;
																		
	  case 1:
		// CEILING
		lastpos = m_Sector->ceilingplane.fD();
		switch (direction)
		{
		case -1:
			// DOWN
			// jff 02/04/98 keep ceiling from moving thru floors
			// [RH] not so easy with arbitrary planes
			//destheight = (dest > m_Sector->floorheight) ? dest : m_Sector->floorheight;
			if (!m_Sector->ceilingplane.isSlope() && !m_Sector->floorplane.isSlope() &&
				(!(i_compatflags2 & COMPATF2_FLOORMOVE) && dest < -m_Sector->floorplane.fD()))
			{
				dest = -m_Sector->floorplane.fD();
			}
			movedest = m_Sector->ceilingplane.GetChangedHeight (-speed);
			if (movedest <= dest)
			{
				move = m_Sector->ceilingplane.HeightDiff (lastpos, dest);

				if (!MoveAttached(crush, move, 1, true)) return crushed;

				m_Sector->ceilingplane.setD(dest);
				flag = P_ChangeSector (m_Sector, crush, move, 1, false);

				if (flag)
				{
					m_Sector->ceilingplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, -move, 1, true);
					MoveAttached(crush, -move, 1, false);
				}
				else
				{
					m_Sector->ChangePlaneTexZ(sector_t::ceiling, move);
				}
				return pastdest;
			}
			else
			{
				if (!MoveAttached(crush, -speed, 1, true)) return crushed;

				m_Sector->ceilingplane.setD(movedest);

				// COULD GET CRUSHED
				flag = P_ChangeSector (m_Sector, crush, -speed, 1, false);
				if (flag)
				{
					if (crush >= 0 && !hexencrush)
					{
						m_Sector->ChangePlaneTexZ(sector_t::ceiling, m_Sector->ceilingplane.HeightDiff (lastpos));
						return crushed;
					}
					m_Sector->ceilingplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, speed, 1, true);
					MoveAttached(crush, speed, 1, false);
					return crushed;
				}
				m_Sector->ChangePlaneTexZ(sector_t::ceiling, m_Sector->ceilingplane.HeightDiff (lastpos));
			}
			break;
												
		case 1:
			// UP
			movedest = m_Sector->ceilingplane.GetChangedHeight (speed);
			if (movedest >= dest)
			{
				move = m_Sector->ceilingplane.HeightDiff (lastpos, dest);

				if (!MoveAttached(crush, move, 1, true)) return crushed;

				m_Sector->ceilingplane.setD(dest);

				flag = P_ChangeSector (m_Sector, crush, move, 1, false);
				if (flag)
				{
					m_Sector->ceilingplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, move, 1, true);
					MoveAttached(crush, move, 1, false);
				}
				else
				{
					m_Sector->ChangePlaneTexZ(sector_t::ceiling, move);
				}
				return pastdest;
			}
			else
			{
				if (!MoveAttached(crush, speed, 1, true)) return crushed;

				m_Sector->ceilingplane.setD(movedest);

				flag = P_ChangeSector (m_Sector, crush, speed, 1, false);
				if (flag)
				{
					m_Sector->ceilingplane.setD(lastpos);
					P_ChangeSector (m_Sector, crush, -speed, 1, true);
					MoveAttached(crush, -speed, 1, false);
					return crushed;
				}
				m_Sector->ChangePlaneTexZ(sector_t::ceiling, m_Sector->ceilingplane.HeightDiff (lastpos));
			}
			break;
		}
		break;
				
	}
	return ok;
}
