#ifndef __RES_SPRITES_H
#define __RES_SPRITES_H

#include "vectors.h"

#define MAX_SPRITE_FRAMES	29		// [RH] Macro-ized as in BOOM.

//
// Sprites are patches with a special naming convention so they can be
// recognized by R_InitSprites. The base name is NNNNFx or NNNNFxFx, with
// x indicating the rotation, x = 0, 1-7. The sprite and frame specified
// by a thing_t is range checked at run time.
// A sprite is a patch_t that is assumed to represent a three dimensional
// object and may have multiple rotations pre drawn. Horizontal flipping
// is used to save space, thus NNNNF2F5 defines a mirrored patch.
// Some sprites will only have one picture used for all views: NNNNF0
//
struct spriteframe_t
{
	struct FVoxelDef *Voxel;// voxel to use for this frame
	FTextureID Texture[16];	// texture to use for view angles 0-15
	WORD Flip;				// flip (1 = flip) to use for view angles 0-15.
};

//
// A sprite definition:
//	a number of animation frames.
//

struct spritedef_t
{
	union
	{
		char name[5];
		DWORD dwName;
	};
	BYTE numframes;
	WORD spriteframes;
};

extern TArray<spriteframe_t> SpriteFrames;


//
// [RH] Internal "skin" definition.
//
class FPlayerSkin
{
public:
	FString		Name;
	FString		Face;
	BYTE		gender = 0;		// This skin's gender (not really used)
	BYTE		range0start = 0;
	BYTE		range0end = 0;
	bool		othergame = 0;	// [GRB]
	DVector2	Scale = { 1, 1 };
	int			sprite = 0;
	int			crouchsprite = 0;
	int			namespc = 0;	// namespace for this skin
};

extern TArray<FPlayerSkin> Skins;

extern BYTE				OtherGameSkinRemap[256];
extern PalEntry			OtherGameSkinPalette[256];

void R_InitSprites ();

#endif
