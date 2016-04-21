
#ifndef __GLC_DATA_H
#define __GLC_DATA_H

#include "doomtype.h"
#include "vectors.h"

struct GLRenderSettings
{

	SBYTE lightmode;
	bool nocoloredspritelighting;
	bool notexturefill;
	bool brightfog;

	SBYTE map_lightmode;
	SBYTE map_nocoloredspritelighting;
	SBYTE map_notexturefill;
	SBYTE map_brightfog;

	FVector3 skyrotatevector;
	FVector3 skyrotatevector2;

	float pixelstretch;

};

extern GLRenderSettings glset;

#include "r_defs.h"
#include "a_sharedglobal.h"
#include "c_cvars.h"

extern int extralight;
EXTERN_CVAR(Int, gl_weaponlight);

inline	int getExtraLight()
{
	return extralight * gl_weaponlight;
}

void gl_RecalcVertexHeights(vertex_t * v);
FTextureID gl_GetSpriteFrame(unsigned sprite, int frame, int rot, angle_t ang, bool *mirror);

class AStackPoint;
struct GLSectorStackPortal;

struct FPortal
{
	DVector2 mDisplacement;
	int plane;
	GLSectorStackPortal *glportal;	// for quick access to the render data. This is only valid during BSP traversal!

	GLSectorStackPortal *GetRenderState();
};

struct FGLLinePortal
{
	// defines the complete span of this portal
	vertex_t	*v1, *v2;	// vertices, from v1 to v2
	DVector2 delta;		// precalculated v2 - v1 for side checking
	FLinePortal *reference;	// one of the associated line portals, for retrieving translation info etc.
};

extern TArray<FPortal *> portals;
extern TArray<FGLLinePortal*> linePortalToGL;

extern TArray<BYTE> currentmapsection;

void gl_InitPortals();
void gl_BuildPortalCoverage(FPortalCoverage *coverage, subsector_t *subsector, const DVector2 &displacement);
void gl_InitData();

extern long gl_frameMS;

#endif
