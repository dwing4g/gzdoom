#include "textures/textures.h"

template<class TYPE> 
void WarpBuffer(TYPE *Pixels, const TYPE *source, int width, int height, int xmul, int ymul, unsigned time, float Speed, int warptype)
{
	int ymask = height - 1;
	int x, y;

	if (warptype == 1)
	{
		TYPE *buffer = (TYPE *)alloca(sizeof(TYPE) * MAX(width, height));
		// [mxd] Rewrote to fix animation for NPo2 textures
		unsigned timebase = unsigned(time * Speed * 32 / 28);
		for (y = height - 1; y >= 0; y--)
		{
			int xf = (TexMan.sintable[((timebase + y*ymul) >> 2)&TexMan.SINMASK] >> 11) % width;
			if (xf < 0) xf += width;
			int xt = xf;
			const TYPE *sourcep = source + y;
			TYPE *dest = Pixels + y;
			for (xt = width; xt; xt--, xf = (xf + 1) % width, dest += height)
				*dest = sourcep[xf + ymask * xf];
		}
		timebase = unsigned(time * Speed * 23 / 28);
		for (x = width - 1; x >= 0; x--)
		{
			int yf = (TexMan.sintable[((time + (x + 17)*xmul) >> 2)&TexMan.SINMASK] >> 11) % height;
			if (yf < 0) yf += height;
			int yt = yf;
			const TYPE *sourcep = Pixels + (x + ymask * x);
			TYPE *dest = buffer;
			for (yt = height; yt; yt--, yf = (yf + 1) % height)
				*dest++ = sourcep[yf];
			memcpy(Pixels + (x + ymask*x), buffer, height * sizeof(TYPE));
		}
	}
	else if (warptype == 2)
	{
		unsigned timebase = unsigned(time * Speed * 40 / 28);
		// [mxd] Rewrote to fix animation for NPo2 textures
		for (x = 0; x < width; x++)
		{
			TYPE *dest = Pixels + (x + ymask * x);
			for (y = 0; y < height; y++)
			{
				int xt = (x + 128
					+ ((TexMan.sintable[((y*ymul + timebase * 5 + 900) >> 2) & TexMan.SINMASK]) >> 13)
					+ ((TexMan.sintable[((x*xmul + timebase * 4 + 300) >> 2) & TexMan.SINMASK]) >> 13)) % width;

				int yt = (y + 128
					+ ((TexMan.sintable[((y*ymul + timebase * 3 + 700) >> 2) & TexMan.SINMASK]) >> 13)
					+ ((TexMan.sintable[((x*xmul + timebase * 4 + 1200) >> 2) & TexMan.SINMASK]) >> 13)) % height;

				*dest++ = source[(xt + ymask * xt) + yt];
			}
		}
	}
	else
	{
		// should never happen, just in case...
		memcpy(Pixels, source, width*height * sizeof(TYPE));
	}
}

