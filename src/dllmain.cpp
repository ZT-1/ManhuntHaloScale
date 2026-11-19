/*
	HaloScale - resolution independent pickup halos for Manhunt (PC)

	The halo wire sphere is emitted by the game as a line list. D3D8 rasterises
	lines exactly one pixel wide no matter how large the backbuffer is, so the
	sphere only keeps its PS2 proportions around 640x448 and the wire gets thinner
	and thinner above that. The radius is world space and scales correctly - it is
	purely the wire that does not. Since the sphere is blended additively at alpha
	120, the dark blue weapon spheres are the first ones to disappear at 1080p.

	PCSX2 is unaffected because upscaling scales line rasterisation along with the
	640x448 geometry, which keeps the ratio intact.

	This plugin replaces the line list with camera facing quads that are a constant
	width in *pixels*, which is what the PS2 output looks like once it is upscaled.

	Standalone: no RenderWare or DirectX SDK needed. It sits alongside MHP.asi and
	touches one call site MHP does not.
*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


/* ---------------------------------------------------------------- addresses */

#define EXE_IMAGE_BASE              0x400000

/* call RwIm3DTransform inside the halo wire sphere strip draw (0x5FBBD0) */
#define ADDR_TRANSFORM_CALLSITE     0x5FBF50

#define ADDR_RwIm3DTransform        0x648550
#define ADDR_RwIm3DRenderIndexed    0x648650
#define ADDR_RwIm3DEnd              0x648620

/*
	Sphere alpha. The wire sphere is blended additively at alpha 120 of 255, so
	additive yellow (which lifts red and green, ~93% of perceived luminance) reads
	fine while additive blue (~7%) barely registers against dark ground. Both of
	these live only in the halo code - the float is referenced from one single
	instruction at 0x5FBC16 - so raising them cannot affect anything else.
*/
#define ADDR_ALPHA_NEAR             0x5FBC64    /* imm32 of mov [esp+1Ch], 120   */
#define ADDR_ALPHA_FADE             0x7CEE6C    /* float 120.0f, distance ramp   */
#define VANILLA_ALPHA               120

#define ADDR_SPHERE_INDICES         0x7CEE28    /* 32 x uint16, 16 line segments */
#define ADDR_SCENE_CAMERA           0x715B94    /* RwCamera **                   */
#define ADDR_SCREEN                 0x7D3440    /* CScreen { float fWidth, fHeight, ... } */

#define CAMERA_FRAME                0x04        /* RwObject::parent -> RwFrame       */
#define CAMERA_VIEWWINDOW_Y         0x6C        /* verified in RwCameraSetViewWindow */

#define FRAME_MATRIX_RIGHT          0x10
#define FRAME_MATRIX_AT             0x30
#define FRAME_MATRIX_POS            0x40

#define PRIMTYPE_TRILIST            3

#define SPHERE_NUM_INDICES          32
#define MAX_LINES                   (SPHERE_NUM_INDICES / 2)


/* -------------------------------------------------------------------- types */

struct Vec3
{
	float x, y, z;
};

/* RwIm3DVertex exactly as the D3D8 driver lays it out - 36 bytes */
struct Im3DVertex
{
	Vec3          objVertex;
	Vec3          objNormal;
	unsigned long color;
	float         u, v;
};

typedef void *(__cdecl *RwIm3DTransform_t)(Im3DVertex *verts, unsigned long numVerts, void *ltm, unsigned long flags);
typedef int   (__cdecl *RwIm3DRenderIndexedPrimitive_t)(int primType, unsigned short *indices, int numIndices);
typedef int   (__cdecl *RwIm3DEnd_t)(void);

static const RwIm3DTransform_t              RwIm3DTransform              = (RwIm3DTransform_t)ADDR_RwIm3DTransform;
static const RwIm3DRenderIndexedPrimitive_t RwIm3DRenderIndexedPrimitive = (RwIm3DRenderIndexedPrimitive_t)ADDR_RwIm3DRenderIndexed;
static const RwIm3DEnd_t                    RwIm3DEnd                    = (RwIm3DEnd_t)ADDR_RwIm3DEnd;


/* ------------------------------------------------------------------ settings */

static struct
{
	int   bEnabled;
	int   nReferenceHeight;
	float fWidthScale;
	float fMaxWidth;
	int   nAlpha;
	int   bLog;
	int   bGlow;
	float fGlowWidth;
	float fGlowAlpha;
	float fBlueAlphaScale;
} Config = { 1, 448, 1.0f, 8.0f, 0, 0, 0, 3.0f, 0.33f, 1.0f };

static char LogPath[MAX_PATH] = { 0 };

/*
	Written once, the first time a halo sphere is actually drawn, so it reports
	what the plugin really saw rather than what the ini asked for.
*/
/*
	Every distinct halo colour actually drawn, in the order first seen.

	Statically the executable only has three CHalos::Add call sites, with the
	constants (20,70,30), (20,70,50) and (20,80,120) - which cannot account for the
	yellow, red, orange and white spheres that clearly appear in game. Recording
	what reaches the renderer settles it without having to find the missing path,
	and catches anything other loaded plugins contribute too.
*/
#define MAX_SEEN_COLORS 64
static unsigned long SeenColors[MAX_SEEN_COLORS];
static int NumSeenColors = 0;

static float LastScreenWidth, LastScreenHeight, LastViewWindowY, LastLineWidth;

static void WriteLog(void);

static void NoteColor(unsigned long color)
{
	unsigned long rgb = color & 0x00FFFFFF;   /* alpha pulses, the hue does not */
	int i;

	for ( i = 0; i < NumSeenColors; i++ )
		if ( SeenColors[i] == rgb ) return;

	if ( NumSeenColors >= MAX_SEEN_COLORS ) return;

	SeenColors[NumSeenColors++] = rgb;
	WriteLog();                               /* rewrite so a crash keeps the list */
}

static void LogOnce(float screenWidth, float screenHeight, float viewWindowY, float lineWidth)
{
	static int done = 0;

	LastScreenWidth = screenWidth;
	LastScreenHeight = screenHeight;
	LastViewWindowY = viewWindowY;
	LastLineWidth = lineWidth;

	if ( done || !Config.bLog || LogPath[0] == 0 ) return;
	done = 1;

	WriteLog();
}

static void WriteLog(void)
{
	FILE *f;
	float screenWidth = LastScreenWidth, screenHeight = LastScreenHeight;
	float viewWindowY = LastViewWindowY, lineWidth = LastLineWidth;
	int i;

	if ( !Config.bLog || LogPath[0] == 0 ) return;

	f = fopen(LogPath, "w");
	if ( f == NULL ) return;

	fprintf(f, "HaloScale\n\n");
	fprintf(f, "game render size   : %.0f x %.0f\n", screenWidth, screenHeight);
	fprintf(f, "camera viewWindow.y: %f\n", viewWindowY);
	fprintf(f, "nReferenceHeight   : %d\n", Config.nReferenceHeight);
	fprintf(f, "fWidthScale        : %f\n", Config.fWidthScale);
	fprintf(f, "computed wire width: %.3f px\n", lineWidth);
	fprintf(f, "quads active       : %s\n", lineWidth > 1.0f ? "YES - drawing quads"
	                                                         : "NO - vanilla line list");
	fprintf(f, "nAlpha             : %d\n", Config.nAlpha);

	fprintf(f, "\nhalo colours seen this session: %d\n", NumSeenColors);
	for ( i = 0; i < NumSeenColors; i++ )
	{
		unsigned long c = SeenColors[i];
		unsigned long r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
		fprintf(f, "  RGB(%3lu,%3lu,%3lu)  #%02lX%02lX%02lX   b-r=%4ld b-g=%4ld |r-g|=%3ld\n",
		        r, g, b, r, g, b,
		        (long)b - (long)r, (long)b - (long)g,
		        (long)(r > g ? r - g : g - r));
	}

	fclose(f);
}

static void LoadConfig(HMODULE self)
{
	char  path[MAX_PATH];
	char  buf[64];
	char *dot;

	if ( !GetModuleFileNameA(self, path, MAX_PATH) ) return;

	dot = strrchr(path, '.');
	if ( dot == NULL ) return;
	strcpy(dot, ".ini");

	Config.bEnabled         = GetPrivateProfileIntA("Main", "bEnabled", Config.bEnabled, path);
	Config.nReferenceHeight = GetPrivateProfileIntA("Main", "nReferenceHeight", Config.nReferenceHeight, path);

	GetPrivateProfileStringA("Main", "fWidthScale", "1.0", buf, sizeof(buf), path);
	Config.fWidthScale = (float)atof(buf);

	GetPrivateProfileStringA("Main", "fMaxWidth", "8.0", buf, sizeof(buf), path);
	Config.fMaxWidth = (float)atof(buf);

	Config.nAlpha = GetPrivateProfileIntA("Main", "nAlpha", Config.nAlpha, path);
	Config.bLog   = GetPrivateProfileIntA("Main", "bLog", Config.bLog, path);
	Config.bGlow  = GetPrivateProfileIntA("Main", "bGlow", Config.bGlow, path);

	GetPrivateProfileStringA("Main", "fGlowWidth", "3.0", buf, sizeof(buf), path);
	Config.fGlowWidth = (float)atof(buf);

	GetPrivateProfileStringA("Main", "fGlowAlpha", "0.33", buf, sizeof(buf), path);
	Config.fGlowAlpha = (float)atof(buf);

	GetPrivateProfileStringA("Main", "fBlueAlphaScale", "1.0", buf, sizeof(buf), path);
	Config.fBlueAlphaScale = (float)atof(buf);

	if ( Config.fGlowWidth < 1.0f )      Config.fGlowWidth = 1.0f;
	if ( Config.fGlowAlpha < 0.0f )      Config.fGlowAlpha = 0.0f;
	if ( Config.fGlowAlpha > 1.0f )      Config.fGlowAlpha = 1.0f;
	if ( Config.fBlueAlphaScale < 0.1f ) Config.fBlueAlphaScale = 0.1f;

	strcpy(LogPath, path);
	dot = strrchr(LogPath, '.');
	if ( dot ) strcpy(dot, ".log");

	if ( Config.nReferenceHeight < 1 ) Config.nReferenceHeight = 448;
	if ( Config.fWidthScale <= 0.0f )  Config.fWidthScale = 1.0f;
	if ( Config.fMaxWidth < 1.0f )     Config.fMaxWidth = 1.0f;
	if ( Config.nAlpha > 255 )         Config.nAlpha = 255;
	if ( Config.nAlpha < 0 )           Config.nAlpha = 0;
}

/*
	Alpha is written into the vertex colour with a shift of 24, so anything above
	255 wraps and the sphere turns transparent instead of brighter.
*/
static void ApplyAlpha(int alpha)
{
	unsigned long protect;
	float alphaFloat = (float)alpha;

	if ( alpha < 1 || alpha > 255 ) return;

	/* only touch it if it still holds the value we expect */
	if ( *(int *)ADDR_ALPHA_NEAR != VANILLA_ALPHA ) return;
	if ( *(float *)ADDR_ALPHA_FADE != (float)VANILLA_ALPHA ) return;

	VirtualProtect((void *)ADDR_ALPHA_NEAR, 4, PAGE_EXECUTE_READWRITE, &protect);
	*(int *)ADDR_ALPHA_NEAR = alpha;
	VirtualProtect((void *)ADDR_ALPHA_NEAR, 4, protect, &protect);

	VirtualProtect((void *)ADDR_ALPHA_FADE, 4, PAGE_READWRITE, &protect);
	*(float *)ADDR_ALPHA_FADE = alphaFloat;
	VirtualProtect((void *)ADDR_ALPHA_FADE, 4, protect, &protect);
}


/* ------------------------------------------------------------------ geometry */

static Im3DVertex     QuadVerts[MAX_LINES * 4];
static unsigned short QuadIndices[MAX_LINES * 6];

static void BuildQuadIndices(void)
{
	for ( int i = 0; i < MAX_LINES; i++ )
	{
		unsigned short *index = &QuadIndices[i * 6];
		unsigned short  base  = (unsigned short)(i * 4);

		index[0] = base + 0;
		index[1] = base + 1;
		index[2] = base + 2;
		index[3] = base + 2;
		index[4] = base + 1;
		index[5] = base + 3;
	}
}

static float GetLineWidth(void)
{
	float screenHeight = *(float *)(ADDR_SCREEN + 4);

	if ( screenHeight < 1.0f ) return 1.0f;

	float width = (screenHeight / (float)Config.nReferenceHeight) * Config.fWidthScale;

	/* around PS2 resolution the vanilla one pixel line is already correct */
	if ( width < 1.1f ) return 1.0f;

	return width > Config.fMaxWidth ? Config.fMaxWidth : width;
}

/*
	Vertex colour is packed 0xAARRGGBB. Scaling only the alpha keeps the hue the
	game chose, which is what identifies the pickup class.

	Blue is singled out because additive blending pays it about 7% of perceived
	luminance where yellow gets ~93%, so an identical alpha reads far weaker on the
	weapon spheres than on anything else. White painkillers sit at r == g == b and
	are deliberately excluded by the margin test - they are already the brightest
	class and the first to clip.
*/
static unsigned long ScaleAlpha(unsigned long color, float scale)
{
	unsigned long a = (color >> 24) & 0xFF;
	unsigned long r = (color >> 16) & 0xFF;
	unsigned long g = (color >> 8) & 0xFF;
	unsigned long b = color & 0xFF;

	/*
		A true blue sphere has red and green near each other and both low - the
		weapon blue measures (38,38,71), so |r-g| is 0. Every cyan-ish class has
		green well clear of red instead: the save point tape is (93,139,171) at
		|r-g| = 46, the quest key (40,67,57) at 27. Testing that second axis keeps
		them apart by 20 or more, where b - g alone put the tape within 2 of the
		threshold and it drifted either side of it depending on distance.
	*/
	if ( b > r + 20 && b > g + 20 && abs((int)r - (int)g) < 20 )
		scale *= Config.fBlueAlphaScale;

	a = (unsigned long)(a * scale + 0.5f);
	if ( a > 255 ) a = 255;

	return (a << 24) | (r << 16) | (g << 8) | b;
}

static int BuildHaloQuads(const Im3DVertex *verts, unsigned long numVerts,
                          float lineWidth, float alphaScale)
{
	void *camera = *(void **)ADDR_SCENE_CAMERA;
	if ( camera == NULL ) return 0;

	void *frame = *(void **)((char *)camera + CAMERA_FRAME);
	if ( frame == NULL ) return 0;

	const Vec3 *cameraRight = (const Vec3 *)((char *)frame + FRAME_MATRIX_RIGHT);
	const Vec3 *cameraAt    = (const Vec3 *)((char *)frame + FRAME_MATRIX_AT);
	const Vec3 *cameraPos   = (const Vec3 *)((char *)frame + FRAME_MATRIX_POS);

	float viewWindowY  = *(float *)((char *)camera + CAMERA_VIEWWINDOW_Y);
	float screenHeight = *(float *)(ADDR_SCREEN + 4);

	/* world units one pixel covers, per unit of view depth */
	const float unitsPerPixel = (2.0f * viewWindowY) / screenHeight;
	const float halfWidth     = 0.5f * lineWidth * unitsPerPixel;

	const unsigned short *indices = (const unsigned short *)ADDR_SPHERE_INDICES;

	int numLines = 0;

	for ( int i = 0; i + 1 < SPHERE_NUM_INDICES; i += 2 )
	{
		if ( numLines >= MAX_LINES ) break;

		unsigned short i0 = indices[i + 0];
		unsigned short i1 = indices[i + 1];

		if ( i0 >= numVerts || i1 >= numVerts ) continue;

		const Vec3 *p0 = &verts[i0].objVertex;
		const Vec3 *p1 = &verts[i1].objVertex;

		Vec3 eyeToStart = { p0->x - cameraPos->x, p0->y - cameraPos->y, p0->z - cameraPos->z };
		Vec3 eyeToEnd   = { p1->x - cameraPos->x, p1->y - cameraPos->y, p1->z - cameraPos->z };
		Vec3 direction  = { p1->x - p0->x,        p1->y - p0->y,        p1->z - p0->z };

		/* a ribbon that always turns its face to the camera */
		Vec3 side;
		side.x = direction.y * eyeToStart.z - direction.z * eyeToStart.y;
		side.y = direction.z * eyeToStart.x - direction.x * eyeToStart.z;
		side.z = direction.x * eyeToStart.y - direction.y * eyeToStart.x;

		float length = (float)sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
		if ( length > 0.000001f )
		{
			float inv = 1.0f / length;
			side.x *= inv;
			side.y *= inv;
			side.z *= inv;
		}
		else
		{
			side = *cameraRight; /* the segment points straight at us, any side does */
		}

		/* widen with distance so the quad stays the same size on screen */
		float startDepth = eyeToStart.x * cameraAt->x + eyeToStart.y * cameraAt->y + eyeToStart.z * cameraAt->z;
		float endDepth   = eyeToEnd.x   * cameraAt->x + eyeToEnd.y   * cameraAt->y + eyeToEnd.z   * cameraAt->z;

		if ( startDepth < 0.0f ) startDepth = 0.0f;
		if ( endDepth   < 0.0f ) endDepth   = 0.0f;

		float sw = halfWidth * startDepth;
		float ew = halfWidth * endDepth;

		Im3DVertex *quad = &QuadVerts[numLines * 4];

		quad[0].objVertex.x = p0->x + side.x * sw;
		quad[0].objVertex.y = p0->y + side.y * sw;
		quad[0].objVertex.z = p0->z + side.z * sw;

		quad[1].objVertex.x = p0->x - side.x * sw;
		quad[1].objVertex.y = p0->y - side.y * sw;
		quad[1].objVertex.z = p0->z - side.z * sw;

		quad[2].objVertex.x = p1->x + side.x * ew;
		quad[2].objVertex.y = p1->y + side.y * ew;
		quad[2].objVertex.z = p1->z + side.z * ew;

		quad[3].objVertex.x = p1->x - side.x * ew;
		quad[3].objVertex.y = p1->y - side.y * ew;
		quad[3].objVertex.z = p1->z - side.z * ew;

		if ( Config.bLog ) NoteColor(verts[i0].color);

		quad[0].color = quad[1].color = ScaleAlpha(verts[i0].color, alphaScale);
		quad[2].color = quad[3].color = ScaleAlpha(verts[i1].color, alphaScale);

		for ( int v = 0; v < 4; v++ )
		{
			quad[v].objNormal.x = quad[v].objNormal.y = quad[v].objNormal.z = 0.0f;
			quad[v].u = quad[v].v = 0.0f;
		}

		numLines++;
	}

	return numLines;
}


/* ---------------------------------------------------------------------- hook */

static void *__cdecl HaloSphereDraw(Im3DVertex *verts, unsigned long numVerts, void *ltm, unsigned long flags)
{
	if ( Config.bEnabled )
	{
		float lineWidth = GetLineWidth();

		if ( Config.bLog )
		{
			void *camera = *(void **)ADDR_SCENE_CAMERA;
			LogOnce(*(float *)ADDR_SCREEN,
			        *(float *)(ADDR_SCREEN + 4),
			        camera ? *(float *)((char *)camera + CAMERA_VIEWWINDOW_Y) : 0.0f,
			        lineWidth);
		}

		if ( lineWidth > 1.0f )
		{
			int numLines;

			/*
				Optional soft pass: a wider, dimmer copy drawn under the sharp wire.
				Additive blending sums the two, reproducing the gentle falloff an
				upscaling emulator gives the PS2 output rather than faking it with
				brute thickness.
			*/
			if ( Config.bGlow && Config.fGlowWidth > 1.0f && Config.fGlowAlpha > 0.0f )
			{
				numLines = BuildHaloQuads(verts, numVerts,
				                          lineWidth * Config.fGlowWidth,
				                          Config.fGlowAlpha);

				if ( numLines > 0 && RwIm3DTransform(QuadVerts, numLines * 4, ltm, flags) )
				{
					RwIm3DRenderIndexedPrimitive(PRIMTYPE_TRILIST, QuadIndices, numLines * 6);
					RwIm3DEnd();
				}
			}

			numLines = BuildHaloQuads(verts, numVerts, lineWidth, 1.0f);

			if ( numLines > 0 && RwIm3DTransform(QuadVerts, numLines * 4, ltm, flags) )
			{
				RwIm3DRenderIndexedPrimitive(PRIMTYPE_TRILIST, QuadIndices, numLines * 6);
				RwIm3DEnd();
			}

			/* the strip is already on screen, make the caller skip its own line list */
			return NULL;
		}
	}

	return RwIm3DTransform(verts, numVerts, ltm, flags);
}


/* --------------------------------------------------------------------- patch */

static bool CallSiteMatches(unsigned int address, unsigned int target)
{
	if ( *(unsigned char *)address != 0xE8 ) return false;

	int rel = *(int *)(address + 1);
	return (unsigned int)(address + 5 + rel) == target;
}

static void RedirectCall(unsigned int address, void *func)
{
	unsigned long protect;
	unsigned char code[5];

	code[0] = 0xE8;
	*(int *)(code + 1) = (int)func - (int)(address + 5);

	VirtualProtect((void *)address, sizeof(code), PAGE_EXECUTE_READWRITE, &protect);
	memcpy((void *)address, code, sizeof(code));
	VirtualProtect((void *)address, sizeof(code), protect, &protect);
}

static void Install(HMODULE self)
{
	/* refuse to touch anything that is not the exe these addresses came from */
	if ( (unsigned int)GetModuleHandleA(NULL) != EXE_IMAGE_BASE ) return;
	if ( !CallSiteMatches(ADDR_TRANSFORM_CALLSITE, ADDR_RwIm3DTransform) ) return;

	LoadConfig(self);
	if ( !Config.bEnabled ) return;

	BuildQuadIndices();
	RedirectCall(ADDR_TRANSFORM_CALLSITE, HaloSphereDraw);

	if ( Config.nAlpha > 0 ) ApplyAlpha(Config.nAlpha);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		DisableThreadLibraryCalls(module);
		Install(module);
	}

	return TRUE;
}
