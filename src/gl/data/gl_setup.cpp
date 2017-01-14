// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_setup.cpp
** Initializes the data structures required by the GL renderer to handle
** a level
**
**/

#include "gl/system/gl_system.h"
#include "doomtype.h"
#include "colormatcher.h"
#include "i_system.h"
#include "p_local.h"
#include "p_spec.h"
#include "p_lnspec.h"
#include "c_dispatch.h"
#include "r_sky.h"
#include "sc_man.h"
#include "w_wad.h"
#include "gi.h"
#include "p_setup.h"
#include "g_level.h"
#include "g_levellocals.h"

#include "gl/renderer/gl_renderer.h"
#include "gl/data/gl_data.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/utility/gl_clock.h"
#include "gl/gl_functions.h"

void InitGLRMapinfoData();

//==========================================================================
//
// 
//
//==========================================================================
static TArray<subsector_t *> MapSectionCollector;

static void DoSetMapSection(subsector_t *sub, int num)
{
	MapSectionCollector.Resize(1);
	MapSectionCollector[0] = sub;
	sub->mapsection = num;
	for (unsigned a = 0; a < MapSectionCollector.Size(); a++)
	{
		sub = MapSectionCollector[a];
		for (DWORD i = 0; i < sub->numlines; i++)
		{
			seg_t * seg = sub->firstline + i;

			if (seg->PartnerSeg)
			{
				subsector_t * sub2 = seg->PartnerSeg->Subsector;

				if (sub2->mapsection != num)
				{
					assert(sub2->mapsection == 0);
					sub2->mapsection = num;
					MapSectionCollector.Push(sub2);
				}
			}
		}
	}
	MapSectionCollector.Clear();
}

//==========================================================================
//
// Merge sections. This is needed in case the map contains errors
// like overlapping lines resulting in abnormal subsectors.
//
// This function ensures that any vertex position can only be in one section.
//
//==========================================================================

struct cvertex_t
{
	double X, Y;

	operator int() const { return xs_FloorToInt(X) + 65536 * xs_FloorToInt(Y); }
	bool operator!= (const cvertex_t &other) const { return fabs(X - other.X) >= EQUAL_EPSILON || fabs(Y - other.Y) >= EQUAL_EPSILON; }
	cvertex_t& operator =(const vertex_t *v) { X = v->fX(); Y = v->fY(); return *this; }
};

typedef TMap<cvertex_t, int> FSectionVertexMap;

static int MergeMapSections(int num)
{
	FSectionVertexMap vmap;
	FSectionVertexMap::Pair *pair;
	TArray<int> sectmap;
	TArray<bool> sectvalid;
	sectmap.Resize(num);
	sectvalid.Resize(num);
	for(int i=0;i<num;i++) 
	{
		sectmap[i] = -1;
		sectvalid[i] = true;
	}
	int mergecount = 1;


	cvertex_t vt;

	// first step: Set mapsection for all vertex positions.
	for(DWORD i=0;i<(DWORD)numsegs;i++)
	{
		seg_t * seg = &segs[i];
		int section = seg->Subsector->mapsection;
		for(int j=0;j<2;j++)
		{
			vt = j==0? seg->v1:seg->v2;
			vmap[vt] = section;
		}
	}

	// second step: Check if any seg references more than one mapsection, either by subsector or by vertex
	for(DWORD i=0;i<(DWORD)numsegs;i++)
	{
		seg_t * seg = &segs[i];
		int section = seg->Subsector->mapsection;
		for(int j=0;j<2;j++)
		{
			vt = j==0? seg->v1:seg->v2;
			int vsection = vmap[vt];

			if (vsection != section)
			{
				// These 2 sections should be merged
				for(int k=0;k<numsubsectors;k++)
				{
					if (subsectors[k].mapsection == vsection) subsectors[k].mapsection = section;
				}
				FSectionVertexMap::Iterator it(vmap);
				while (it.NextPair(pair))
				{
					if (pair->Value == vsection) pair->Value = section;
				}
				sectvalid[vsection-1] = false;
			}
		}
	}
	for(int i=0;i<num;i++)
	{
		if (sectvalid[i]) sectmap[i] = mergecount++;
	}
	for(int i=0;i<numsubsectors;i++)
	{
		subsectors[i].mapsection = sectmap[subsectors[i].mapsection-1];
		assert(subsectors[i].mapsection!=-1);
	}
	return mergecount-1;
}

//==========================================================================
//
// 
//
//==========================================================================

static void SetMapSections()
{
	bool set;
	int num = 0;
	do
	{
		set = false;
		for(int i=0; i<numsubsectors; i++)
		{
			if (subsectors[i].mapsection == 0)
			{
				num++;
				DoSetMapSection(&subsectors[i], num);
				set = true;
				break;
			}
		}
	}
	while (set);
	num = MergeMapSections(num);
	currentmapsection.Resize(1 + num/8);
#ifdef DEBUG
	Printf("%d map sections found\n", num);
#endif
}

//==========================================================================
//
// prepare subsectors for GL rendering
// - analyze rendering hacks using open sectors
// - assign a render sector (for self referencing sectors)
// - calculate a bounding box
//
//==========================================================================

static void SpreadHackedFlag(subsector_t * sub)
{
	// The subsector pointer hasn't been set yet!
	for(DWORD i=0;i<sub->numlines;i++)
	{
		seg_t * seg = sub->firstline + i;

		if (seg->PartnerSeg)
		{
			subsector_t * sub2 = seg->PartnerSeg->Subsector;

			if (!(sub2->hacked&1) && sub2->render_sector == sub->render_sector)
			{
				sub2->hacked|=1;
				sub->hacked &= ~4;
				SpreadHackedFlag (sub2);
			}
		}
	}
}


//==========================================================================
//
// 
//
//==========================================================================

static void PrepareSectorData()
{
	int 				i;
	TArray<subsector_t *> undetermined;
	subsector_t *		ss;

	// now group the subsectors by sector
	subsector_t ** subsectorbuffer = new subsector_t * [numsubsectors];

	for(i=0, ss=subsectors; i<numsubsectors; i++, ss++)
	{
		ss->render_sector->subsectorcount++;
	}

	for (auto &sec : level.sectors) 
	{
		sec.subsectors = subsectorbuffer;
		subsectorbuffer += sec.subsectorcount;
		sec.subsectorcount = 0;
	}
	
	for(i=0, ss = subsectors; i<numsubsectors; i++, ss++)
	{
		ss->render_sector->subsectors[ss->render_sector->subsectorcount++]=ss;
	}

	// marks all malformed subsectors so rendering tricks using them can be handled more easily
	for (i = 0; i < numsubsectors; i++)
	{
		if (subsectors[i].sector == subsectors[i].render_sector)
		{
			seg_t * seg = subsectors[i].firstline;
			for(DWORD j=0;j<subsectors[i].numlines;j++)
			{
				if (!(subsectors[i].hacked&1) && seg[j].linedef==0 && 
						seg[j].PartnerSeg!=NULL && 
						subsectors[i].render_sector != seg[j].PartnerSeg->Subsector->render_sector)
				{
					DPrintf(DMSG_NOTIFY, "Found hack: (%f,%f) (%f,%f)\n", seg[j].v1->fX(), seg[j].v1->fY(), seg[j].v2->fX(), seg[j].v2->fY());
					subsectors[i].hacked|=5;
					SpreadHackedFlag(&subsectors[i]);
				}
				if (seg[j].PartnerSeg==NULL) subsectors[i].hacked|=2;	// used for quick termination checks
			}
		}
	}
	SetMapSections();
}

//==========================================================================
//
// Some processing for transparent door hacks using a floor raised by 1 map unit
// - This will be used to lower the floor of such sectors by one map unit
//
//==========================================================================

static void PrepareTransparentDoors(sector_t * sector)
{
	bool solidwall=false;
	unsigned int notextures=0;
	unsigned int nobtextures=0;
	unsigned int selfref=0;
	sector_t * nextsec=NULL;

	P_Recalculate3DFloors(sector);
	if (sector->subsectorcount==0) return;

	sector->transdoorheight=sector->GetPlaneTexZ(sector_t::floor);
	sector->transdoor= !(sector->e->XFloor.ffloors.Size() || sector->heightsec || sector->floorplane.isSlope());

	if (sector->transdoor)
	{
		for (auto ln : sector->Lines)
		{
			if (ln->frontsector == ln->backsector)
			{
				selfref++;
				continue;
			}

			sector_t * sec=getNextSector(ln, sector);
			if (sec==NULL) 
			{
				solidwall=true;
				continue;
			}
			else
			{
				nextsec=sec;

				int side = ln->sidedef[0]->sector == sec;

				if (sector->GetPlaneTexZ(sector_t::floor)!=sec->GetPlaneTexZ(sector_t::floor)+1. || sec->floorplane.isSlope())
				{
					sector->transdoor=false;
					return;
				}
				if (!ln->sidedef[1-side]->GetTexture(side_t::top).isValid()) notextures++;
				if (!ln->sidedef[1-side]->GetTexture(side_t::bottom).isValid()) nobtextures++;
			}
		}
		if (sector->GetTexture(sector_t::ceiling)==skyflatnum)
		{
			sector->transdoor=false;
			return;
		}

		if (selfref+nobtextures!=sector->Lines.Size())
		{
			sector->transdoor=false;
		}

		if (selfref+notextures!=sector->Lines.Size())
		{
			// This is a crude attempt to fix an incorrect transparent door effect I found in some
			// WolfenDoom maps but considering the amount of code required to handle it I left it in.
			// Do this only if the sector only contains one-sided walls or ones with no lower texture.
			if (solidwall)
			{
				if (solidwall+nobtextures+selfref==sector->Lines.Size() && nextsec)
				{
					sector->heightsec=nextsec;
					sector->heightsec->MoreFlags=0;
				}
				sector->transdoor=false;
			}
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

static void AddToVertex(const sector_t * sec, TArray<int> & list)
{
	int secno = sec->Index();

	for(unsigned i=0;i<list.Size();i++)
	{
		if (list[i]==secno) return;
	}
	list.Push(secno);
}

//==========================================================================
//
// Attach sectors to vertices - used to generate vertex height lists
//
//==========================================================================

static void InitVertexData()
{
	TArray<int> * vt_sectorlists;

	int i,j,k;

	vt_sectorlists = new TArray<int>[level.vertexes.Size()];


	for(auto &line : level.lines)
	{
		for(j=0;j<2;j++)
		{
			vertex_t * v = j==0? line.v1 : line.v2;

			for(k=0;k<2;k++)
			{
				sector_t * sec = k==0? line.frontsector : line.backsector;

				if (sec)
				{
					extsector_t::xfloor &x = sec->e->XFloor;

					AddToVertex(sec, vt_sectorlists[v->Index()]);
					if (sec->heightsec) AddToVertex(sec->heightsec, vt_sectorlists[v->Index()]);
				}
			}
		}
	}

	for(i=0;i<level.vertexes.Size();i++)
	{
		auto vert = level.vertexes[i];
		int cnt = vt_sectorlists[i].Size();

		vert.dirty = true;
		vert.numheights=0;
		if (cnt>1)
		{
			vert.numsectors= cnt;
			vert.sectors=new sector_t*[cnt];
			vert.heightlist = new float[cnt*2];
			for(int j=0;j<cnt;j++)
			{
				vert.sectors[j] = &level.sectors[vt_sectorlists[i][j]];
			}
		}
		else
		{
			vert.numsectors=0;
		}
	}

	delete [] vt_sectorlists;
}

//==========================================================================
//
//
//
//==========================================================================

static void GetSideVertices(int sdnum, DVector2 *v1, DVector2 *v2)
{
	line_t *ln = level.sides[sdnum].linedef;
	if (ln->sidedef[0] == &level.sides[sdnum])
	{
		*v1 = ln->v1->fPos();
		*v2 = ln->v2->fPos();
	}
	else
	{
		*v2 = ln->v1->fPos();
		*v1 = ln->v2->fPos();
	}
}

static int segcmp(const void *a, const void *b)
{
	seg_t *A = *(seg_t**)a;
	seg_t *B = *(seg_t**)b;
	return xs_RoundToInt(FRACUNIT*(A->sidefrac - B->sidefrac));
}

//==========================================================================
//
// Group segs to sidedefs
//
//==========================================================================

static void PrepareSegs()
{
	auto numsides = level.sides.Size();
	int *segcount = new int[numsides];
	int realsegs = 0;

	// Get floatng point coordinates of vertices
	for(auto &v : level.vertexes)
	{
		v.dirty = true;
	}

	// count the segs
	memset(segcount, 0, numsides * sizeof(int));

	for(int i=0;i<numsegs;i++)
	{
		seg_t *seg = &segs[i];

		if (seg->sidedef == NULL) continue;	// miniseg
		int sidenum = seg->sidedef->Index();

		realsegs++;
		segcount[sidenum]++;
		DVector2 sidestart, sideend, segend = seg->v2->fPos();
		GetSideVertices(sidenum, &sidestart, &sideend);

		sideend -=sidestart;
		segend -= sidestart;

		seg->sidefrac = float(segend.Length() / sideend.Length());
	}

	// allocate memory
	level.sides[0].segs = new seg_t*[realsegs];
	level.sides[0].numsegs = 0;

	for(int i = 1; i < numsides; i++)
	{
		level.sides[i].segs = level.sides[i-1].segs + segcount[i-1];
		level.sides[i].numsegs = 0;
	}
	delete [] segcount;

	// assign the segs
	for(int i=0;i<numsegs;i++)
	{
		seg_t *seg = &segs[i];
		if (seg->sidedef != NULL) seg->sidedef->segs[seg->sidedef->numsegs++] = seg;
	}

	// sort the segs
	for(int i = 0; i < numsides; i++)
	{
		if (level.sides[i].numsegs > 1) qsort(level.sides[i].segs, level.sides[i].numsegs, sizeof(seg_t*), segcmp);
	}
}

//==========================================================================
//
// Initialize the level data for the GL renderer
//
//==========================================================================
extern int restart;

void gl_PreprocessLevel()
{
	PrepareSegs();
	PrepareSectorData();
	InitVertexData();
	int *checkmap = new int[level.vertexes.Size()];
	memset(checkmap, -1, sizeof(int)*level.vertexes.Size());
	for(auto &sec : level.sectors) 
	{
		int i = sec.sectornum;
		PrepareTransparentDoors(&sec);

		// This ignores vertices only used for seg splitting because those aren't needed here
		for(auto l : sec.Lines)
		{
			if (l->sidedef[0]->Flags & WALLF_POLYOBJ) continue;	// don't bother with polyobjects

			int vtnum1 = l->v1->Index();
			int vtnum2 = l->v2->Index();

			if (checkmap[vtnum1] < i)
			{
				checkmap[vtnum1] = i;
				sec.e->vertices.Push(&level.vertexes[vtnum1]);
				level.vertexes[vtnum1].dirty = true;
			}

			if (checkmap[vtnum2] < i)
			{
				checkmap[vtnum2] = i;
				sec.e->vertices.Push(&level.vertexes[vtnum2]);
				level.vertexes[vtnum2].dirty = true;
			}
		}
	}
	delete[] checkmap;

	gl_InitPortals();

	if (GLRenderer != NULL) 
	{
		GLRenderer->SetupLevel();
	}

#if 0
	gl_CreateSections();
#endif

	InitGLRMapinfoData();
}



//==========================================================================
//
// Cleans up all the GL data for the last level
//
//==========================================================================

void gl_CleanLevelData()
{
	// Dynamic lights must be destroyed before the sector information here is deleted.
	TThinkerIterator<ADynamicLight> it(STAT_DLIGHT);
	AActor * mo=it.Next();
	while (mo)
	{
		AActor * next = it.Next();
		mo->Destroy();
		mo=next;
	}

	if (level.sides.Size() > 0 && level.sides[0].segs)
	{
		delete [] level.sides[0].segs;
		level.sides[0].segs = NULL;
	}
	if (level.sectors.Size() > 0 && level.sectors[0].subsectors) 
	{
		delete [] level.sectors[0].subsectors;
		level.sectors[0].subsectors = nullptr;
	}
	for (int i=0;i<numsubsectors;i++)
	{
		for(int j=0;j<2;j++)
		{
			if (subsectors[i].portalcoverage[j].subsectors != NULL)
			{
				delete [] subsectors[i].portalcoverage[j].subsectors;
				subsectors[i].portalcoverage[j].subsectors = NULL;
			}
		}
	}
	for(unsigned i=0;i<portals.Size(); i++)
	{
		delete portals[i];
	}
	portals.Clear();
}


//==========================================================================
//
//
//
//==========================================================================

CCMD(listmapsections)
{
	for(int i=0;i<100;i++)
	{
		for (int j=0;j<numsubsectors;j++)
		{
			if (subsectors[j].mapsection == i)
			{
				Printf("Mapsection %d, sector %d, line %d\n", i, subsectors[j].render_sector->Index(), subsectors[j].firstline->linedef->Index());
				break;
			}
		}
	}
}
