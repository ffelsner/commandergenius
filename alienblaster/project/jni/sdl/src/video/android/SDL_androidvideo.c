/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2009 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

/* Dummy SDL video driver implementation; this is just enough to make an
 *  SDL-based application THINK it's got a working video driver, for
 *  applications that call SDL_Init(SDL_INIT_VIDEO) when they don't need it,
 *  and also for use as a collection of stubs when porting SDL to a new
 *  platform for which you haven't yet written a valid video driver.
 *
 * This is also a great way to determine bottlenecks: if you think that SDL
 *  is a performance problem for a given platform, enable this driver, and
 *  then see if your application runs faster without video overhead.
 *
 * Initial work by Ryan C. Gordon (icculus@icculus.org). A good portion
 *  of this was cut-and-pasted from Stephane Peter's work in the AAlib
 *  SDL video driver.  Renamed to "ANDROID" by Sam Lantinga.
 */

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_mutex.h"
#include "SDL_thread.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_androidvideo.h"

#include <jni.h>
#include <android/log.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include <string.h> // for memset()


#define ANDROIDVID_DRIVER_NAME "android"

/* Initialization/Query functions */
static int ANDROID_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **ANDROID_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *ANDROID_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int ANDROID_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void ANDROID_VideoQuit(_THIS);

/* Hardware surface functions */
static int ANDROID_AllocHWSurface(_THIS, SDL_Surface *surface);
static int ANDROID_LockHWSurface(_THIS, SDL_Surface *surface);
static void ANDROID_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void ANDROID_FreeHWSurface(_THIS, SDL_Surface *surface);
static int ANDROID_FlipHWSurface(_THIS, SDL_Surface *surface);
static void ANDROID_GL_SwapBuffers(_THIS);

// Stubs to get rid of crashing in OpenGL mode
// The implementation dependent data for the window manager cursor
struct WMcursor {
    int unused ;
};

void ANDROID_FreeWMCursor(_THIS, WMcursor *cursor) {
    SDL_free (cursor);
    return;
}
WMcursor * ANDROID_CreateWMCursor(_THIS, Uint8 *data, Uint8 *mask, int w, int h, int hot_x, int hot_y) {
    WMcursor * cursor;
    cursor = (WMcursor *) SDL_malloc (sizeof (WMcursor)) ;
    if (cursor == NULL) {
        SDL_OutOfMemory () ;
        return NULL ;
    }
    return cursor;
}
int ANDROID_ShowWMCursor(_THIS, WMcursor *cursor) {
    return 1;
}
void ANDROID_WarpWMCursor(_THIS, Uint16 x, Uint16 y) { }
void ANDROID_MoveWMCursor(_THIS, int x, int y) { }


/* etc. */
static void ANDROID_UpdateRects(_THIS, int numrects, SDL_Rect *rects);


/* Private display data */

#define SDL_NUMMODES 4
struct SDL_PrivateVideoData {
	SDL_Rect *SDL_modelist[SDL_NUMMODES+1];
};

#define SDL_modelist		(this->hidden->SDL_modelist)


// The device screen dimensions to draw on
static int sWindowWidth  = 320;
static int sWindowHeight = 480;
// Pointer to in-memory video surface
static int memX = 0;
static int memY = 0;
// In-memory surfaces
static void * memBuffer1 = NULL;
static void * memBuffer2 = NULL;
static void * memBuffer = NULL;
static int sdl_opengl = 0;
// Some wicked GLES stuff
static GLuint texture = 0;

// Extremely wicked JNI environment to call Java functions from C code
static JNIEnv* JavaEnv = NULL;
static jclass JavaRendererClass = NULL;
static jobject JavaRenderer = NULL;
static jmethodID JavaSwapBuffers = NULL;


static SDLKey keymap[KEYCODE_LAST+1];

static int CallJavaSwapBuffers();
static void SdlGlRenderInit();
static int processAndroidTrackballKeyDelays( int key, int action );

/* ANDROID driver bootstrap functions */

static int ANDROID_Available(void)
{
	/*
	const char *envr = SDL_getenv("SDL_VIDEODRIVER");
	if ((envr) && (SDL_strcmp(envr, ANDROIDVID_DRIVER_NAME) == 0)) {
		return(1);
	}

	return(0);
	*/
	return 1;
}

static void ANDROID_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *ANDROID_CreateDevice(int devindex)
{
	SDL_VideoDevice *device;

	/* Initialize all variables that we clean on shutdown */
	device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( device ) {
		SDL_memset(device, 0, (sizeof *device));
		device->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *device->hidden));
	}
	if ( (device == NULL) || (device->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( device ) {
			SDL_free(device);
		}
		return(0);
	}
	SDL_memset(device->hidden, 0, (sizeof *device->hidden));

	/* Set the function pointers */
	device->VideoInit = ANDROID_VideoInit;
	device->ListModes = ANDROID_ListModes;
	device->SetVideoMode = ANDROID_SetVideoMode;
	device->CreateYUVOverlay = NULL;
	device->SetColors = ANDROID_SetColors;
	device->UpdateRects = ANDROID_UpdateRects;
	device->VideoQuit = ANDROID_VideoQuit;
	device->AllocHWSurface = ANDROID_AllocHWSurface;
	device->CheckHWBlit = NULL;
	device->FillHWRect = NULL;
	device->SetHWColorKey = NULL;
	device->SetHWAlpha = NULL;
	device->LockHWSurface = ANDROID_LockHWSurface;
	device->UnlockHWSurface = ANDROID_UnlockHWSurface;
	device->FlipHWSurface = ANDROID_FlipHWSurface;
	device->FreeHWSurface = ANDROID_FreeHWSurface;
	device->SetCaption = NULL;
	device->SetIcon = NULL;
	device->IconifyWindow = NULL;
	device->GrabInput = NULL;
	device->GetWMInfo = NULL;
	device->InitOSKeymap = ANDROID_InitOSKeymap;
	device->PumpEvents = ANDROID_PumpEvents;
	device->GL_SwapBuffers = ANDROID_GL_SwapBuffers;
	device->free = ANDROID_DeleteDevice;

	// Stubs
	device->FreeWMCursor = ANDROID_FreeWMCursor;
	device->CreateWMCursor = ANDROID_CreateWMCursor;
	device->ShowWMCursor = ANDROID_ShowWMCursor;
	device->WarpWMCursor = ANDROID_WarpWMCursor;
	device->MoveWMCursor = ANDROID_MoveWMCursor;

	return device;
}

VideoBootStrap ANDROID_bootstrap = {
	ANDROIDVID_DRIVER_NAME, "SDL android video driver",
	ANDROID_Available, ANDROID_CreateDevice
};


int ANDROID_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	int i;
	/* Determine the screen depth (use default 16-bit depth) */
	/* we change this during the SDL_SetVideoMode implementation... */
	vformat->BitsPerPixel = 16;
	vformat->BytesPerPixel = 2;

	for ( i=0; i<SDL_NUMMODES; ++i ) {
		SDL_modelist[i] = SDL_malloc(sizeof(SDL_Rect));
		SDL_modelist[i]->x = SDL_modelist[i]->y = 0;
	}
	/* Modes sorted largest to smallest */
	SDL_modelist[0]->w = sWindowWidth; SDL_modelist[0]->h = sWindowHeight;
	SDL_modelist[1]->w = 640; SDL_modelist[1]->h = 480; // Will likely be shrinked
	SDL_modelist[2]->w = 320; SDL_modelist[2]->h = 240; // Always available on any screen and any orientation
	SDL_modelist[3]->w = 320; SDL_modelist[3]->h = 200; // Always available on any screen and any orientation
	SDL_modelist[4] = NULL;

	/* We're done! */
	return(0);
}

SDL_Rect **ANDROID_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	if(format->BitsPerPixel != 16)
		return NULL;
	return SDL_modelist;
}

SDL_Surface *ANDROID_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
    __android_log_print(ANDROID_LOG_INFO, "libSDL", "SDL_SetVideoMode(): application requested mode %dx%d", width, height);

	if ( memBuffer1 )
		SDL_free( memBuffer1 );
	if ( memBuffer2 )
		SDL_free( memBuffer2 );

	memBuffer = memBuffer1 = memBuffer2 = NULL;

	sdl_opengl = (flags & SDL_OPENGL) ? 1 : 0;

	memX = width;
	memY = height;
	
	if( ! sdl_opengl )
	{
		memBuffer1 = SDL_malloc(memX * memY * (bpp / 8));
		if ( ! memBuffer1 ) {
			__android_log_print(ANDROID_LOG_INFO, "libSDL", "Couldn't allocate buffer for requested mode");
			SDL_SetError("Couldn't allocate buffer for requested mode");
			return(NULL);
		}
		SDL_memset(memBuffer1, 0, memX * memY * (bpp / 8));

		if( flags & SDL_DOUBLEBUF )
		{
			memBuffer2 = SDL_malloc(memX * memY * (bpp / 8));
			if ( ! memBuffer2 ) {
				__android_log_print(ANDROID_LOG_INFO, "libSDL", "Couldn't allocate buffer for requested mode");
				SDL_SetError("Couldn't allocate buffer for requested mode");
				return(NULL);
			}
			SDL_memset(memBuffer2, 0, memX * memY * (bpp / 8));
		}
		memBuffer = memBuffer1;
	}

	/* Allocate the new pixel format for the screen */
	if ( ! SDL_ReallocFormat(current, bpp, 0, 0, 0, 0) ) {
		if(memBuffer)
			SDL_free(memBuffer);
		memBuffer = NULL;
		__android_log_print(ANDROID_LOG_INFO, "libSDL", "Couldn't allocate new pixel format for requested mode");
		SDL_SetError("Couldn't allocate new pixel format for requested mode");
		return(NULL);
	}

	/* Set up the new mode framebuffer */
	current->flags = (flags & SDL_FULLSCREEN) | (flags & SDL_DOUBLEBUF) | (flags & SDL_OPENGL);
	current->w = width;
	current->h = height;
	current->pitch = memX * (bpp / 8);
	current->pixels = memBuffer;
	
	SdlGlRenderInit();

	/* We're done */
	return(current);
}

/* Note:  If we are terminated, this could be called in the middle of
   another SDL video routine -- notably UpdateRects.
*/
void ANDROID_VideoQuit(_THIS)
{
	if( ! sdl_opengl )
	{
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDeleteTextures(1, &texture);
	}

	memX = 0;
	memY = 0;
	memBuffer = NULL;
	SDL_free( memBuffer1 );
	memBuffer1 = NULL;
	if( memBuffer2 )
		SDL_free( memBuffer2 );
	memBuffer2 = NULL;

	int i;
	
	if (this->screen->pixels != NULL)
	{
		SDL_free(this->screen->pixels);
		this->screen->pixels = NULL;
	}
	/* Free video mode lists */
	for ( i=0; i<SDL_NUMMODES; ++i ) {
		if ( SDL_modelist[i] != NULL ) {
			SDL_free(SDL_modelist[i]);
			SDL_modelist[i] = NULL;
		}
	}
}

void ANDROID_PumpEvents(_THIS)
{
}

/* We don't actually allow hardware surfaces other than the main one */
// TODO: use OpenGL textures here
static int ANDROID_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	return(-1);
}
static void ANDROID_FreeHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

/* We need to wait for vertical retrace on page flipped displays */
static int ANDROID_LockHWSurface(_THIS, SDL_Surface *surface)
{
	return(0);
}

static void ANDROID_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

static void ANDROID_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	ANDROID_FlipHWSurface(this, SDL_VideoSurface);
}

static int ANDROID_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	if( ! sdl_opengl )
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, memX, memY, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, memBuffer);
		if( sWindowHeight < memY || sWindowWidth < memX )
			glDrawTexiOES(0, 0, 1, sWindowWidth, sWindowHeight);  // Larger than screen - shrink to fit
		else
			glDrawTexiOES(0, sWindowHeight-memY, 1, memX, memY);  // Smaller than screen - do not scale, it's faster that way

		if( surface->flags & SDL_DOUBLEBUF )
		{
			if( memBuffer == memBuffer1 )
				memBuffer = memBuffer2;
			else
				memBuffer = memBuffer1;
			surface->pixels = memBuffer;
		}
	}

	CallJavaSwapBuffers();

	processAndroidTrackballKeyDelays( -1, 0 );

	SDL_Delay(10);
	
	return(0);
};

void ANDROID_GL_SwapBuffers(_THIS)
{
	ANDROID_FlipHWSurface(this, NULL);
};

int ANDROID_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	return(1);
}

/* JNI-C++ wrapper stuff */

#ifndef SDL_JAVA_PACKAGE_PATH
#error You have to define SDL_JAVA_PACKAGE_PATH to your package path with dots replaced with underscores, for example "com_example_SanAngeles"
#endif
#define JAVA_EXPORT_NAME2(name,package) Java_##package##_##name
#define JAVA_EXPORT_NAME1(name,package) JAVA_EXPORT_NAME2(name,package)
#define JAVA_EXPORT_NAME(name) JAVA_EXPORT_NAME1(name,SDL_JAVA_PACKAGE_PATH)

extern void
JAVA_EXPORT_NAME(DemoRenderer_nativeResize) ( JNIEnv*  env, jobject  thiz, jint w, jint h )
{
    sWindowWidth  = w;
    sWindowHeight = h;
    __android_log_print(ANDROID_LOG_INFO, "libSDL", "Physical screen resolution is %dx%d", w, h);
}

/* Call to finalize the graphics state */
extern void
JAVA_EXPORT_NAME(DemoRenderer_nativeDone) ( JNIEnv*  env, jobject  thiz )
{
	__android_log_print(ANDROID_LOG_INFO, "libSDL", "quitting...");
	SDL_PrivateQuit();
	__android_log_print(ANDROID_LOG_INFO, "libSDL", "quit OK");
}

extern void
JAVA_EXPORT_NAME(AccelerometerReader_nativeAccelerometer) ( JNIEnv*  env, jobject  thiz, jfloat accX, jfloat accY, jfloat accZ )
{
	// TODO: use accelerometer as joystick
}

enum MOUSE_ACTION { MOUSE_DOWN = 0, MOUSE_UP=1, MOUSE_MOVE=2 };

extern void
JAVA_EXPORT_NAME(DemoGLSurfaceView_nativeMouse) ( JNIEnv*  env, jobject  thiz, jint x, jint y, jint action )
{
	//__android_log_print(ANDROID_LOG_INFO, "libSDL", "mouse event %i at (%03i, %03i)", action, x, y);
	if( action == MOUSE_DOWN || action == MOUSE_UP )
		SDL_PrivateMouseButton( (action == MOUSE_DOWN) ? SDL_PRESSED : SDL_RELEASED, 1, x, y );
	if( action == MOUSE_MOVE )
		SDL_PrivateMouseMotion(0, 0, x, y);
}

static SDL_keysym *TranslateKey(int scancode, SDL_keysym *keysym)
{
	/* Sanity check */
	if ( scancode >= SDL_arraysize(keymap) )
		scancode = KEYCODE_UNKNOWN;

	/* Set the keysym information */
	keysym->scancode = scancode;
	keysym->sym = keymap[scancode];
	keysym->mod = KMOD_NONE;

	/* If UNICODE is on, get the UNICODE value for the key */
	keysym->unicode = 0;
	if ( SDL_TranslateUNICODE ) {
		/* Populate the unicode field with the ASCII value */
		keysym->unicode = scancode;
	}
	return(keysym);
}


void
JAVA_EXPORT_NAME(DemoGLSurfaceView_nativeKey) ( JNIEnv*  env, jobject thiz, jint key, jint action )
{
	//__android_log_print(ANDROID_LOG_INFO, "libSDL", "key event %i %s", key, action ? "down" : "up");
	SDL_keysym keysym;
	if( ! processAndroidTrackballKeyDelays(key, action) )
		SDL_PrivateKeyboard( action ? SDL_PRESSED : SDL_RELEASED, TranslateKey(key, &keysym) );
}

void SdlGlRenderInit()
{
	// Set up an array of values to use as the sprite vertices.
	static GLfloat vertices[] =
	{
		0, 0,
		1, 0,
		0, 1,
		1, 1,
	};
	
	// Set up an array of values for the texture coordinates.
	static GLfloat texcoords[] =
	{
		0, 0,
		1, 0,
		0, 1,
		1, 1,
	};
	
	static GLint texcoordsCrop[] =
	{
		0, 0, 0, 0,
	};
	
	static float clearColor = 0.0f;
	static int clearColorDir = 1;
	int textX, textY;
	void * memBufferTemp;
	
	if( !sdl_opengl && memBuffer )
	{
			// Texture sizes should be 2^n
			textX = memX;
			textY = memY;

			if( textX <= 256 )
				textX = 256;
			else if( textX <= 512 )
				textX = 512;
			else
				textX = 1024;

			if( textY <= 256 )
				textY = 256;
			else if( textY <= 512 )
				textY = 512;
			else
				textY = 1024;

			glViewport(0, 0, textX, textY);

			glClearColor(0,0,0,0);
			// Set projection
			glMatrixMode( GL_PROJECTION );
			glLoadIdentity();
			#if defined(GL_VERSION_ES_CM_1_0)
				#define glOrtho glOrthof
			#endif
			glOrtho( 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f );

			// Now Initialize modelview matrix
			glMatrixMode( GL_MODELVIEW );
			glLoadIdentity();
			
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_CULL_FACE);
			glDisable(GL_DITHER);
			glDisable(GL_MULTISAMPLE);

			glEnable(GL_TEXTURE_2D);
			
			glGenTextures(1, &texture);

			glBindTexture(GL_TEXTURE_2D, texture);
	
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

			void * textBuffer = SDL_malloc( textX*textY*2 );
			SDL_memset( textBuffer, 0, textX*textY*2 );
			
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, textX, textY, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, textBuffer);

			glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);

			glVertexPointer(2, GL_FLOAT, 0, vertices);
			glTexCoordPointer(2, GL_FLOAT, 0, texcoords);

			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

			texcoordsCrop[0] = 0;
			texcoordsCrop[1] = memY;
			texcoordsCrop[2] = memX;
			texcoordsCrop[3] = -memY;

			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, texcoordsCrop);
			
			glFinish();
			
			SDL_free( textBuffer );
	}
}


void
JAVA_EXPORT_NAME(DemoRenderer_nativeInitJavaCallbacks) ( JNIEnv*  env, jobject thiz )
{
	char classPath[1024];
	JavaEnv = env;
	JavaRenderer = thiz;
	
	JavaRendererClass = (*JavaEnv)->GetObjectClass(JavaEnv, thiz);
	JavaSwapBuffers = (*JavaEnv)->GetMethodID(JavaEnv, JavaRendererClass, "swapBuffers", "()I");
}

int CallJavaSwapBuffers()
{
	return (*JavaEnv)->CallIntMethod( JavaEnv, JavaRenderer, JavaSwapBuffers );
}

void ANDROID_InitOSKeymap(_THIS)
{
  int i;
	
  /* Initialize the DirectFB key translation table */
  for (i=0; i<SDL_arraysize(keymap); ++i)
    keymap[i] = SDLK_UNKNOWN;

  keymap[KEYCODE_UNKNOWN] = SDLK_UNKNOWN;

  keymap[KEYCODE_BACK] = SDLK_ESCAPE;

  keymap[KEYCODE_MENU] = SDLK_LALT;
  keymap[KEYCODE_CALL] = SDLK_LCTRL;
  keymap[KEYCODE_ENDCALL] = SDLK_LSHIFT;
  keymap[KEYCODE_CAMERA] = SDLK_RSHIFT;
  keymap[KEYCODE_POWER] = SDLK_RALT;

  keymap[KEYCODE_BACK] = SDLK_ESCAPE; // Note: generates SDL_QUIT
  keymap[KEYCODE_0] = SDLK_0;
  keymap[KEYCODE_1] = SDLK_1;
  keymap[KEYCODE_2] = SDLK_2;
  keymap[KEYCODE_3] = SDLK_3;
  keymap[KEYCODE_4] = SDLK_4;
  keymap[KEYCODE_5] = SDLK_5;
  keymap[KEYCODE_6] = SDLK_6;
  keymap[KEYCODE_7] = SDLK_7;
  keymap[KEYCODE_8] = SDLK_8;
  keymap[KEYCODE_9] = SDLK_9;
  keymap[KEYCODE_STAR] = SDLK_ASTERISK;
  keymap[KEYCODE_POUND] = SDLK_DOLLAR;

  keymap[KEYCODE_DPAD_UP] = SDLK_UP;
  keymap[KEYCODE_DPAD_DOWN] = SDLK_DOWN;
  keymap[KEYCODE_DPAD_LEFT] = SDLK_LEFT;
  keymap[KEYCODE_DPAD_RIGHT] = SDLK_RIGHT;
  keymap[KEYCODE_DPAD_CENTER] = SDLK_RETURN;

  keymap[KEYCODE_SOFT_LEFT] = SDLK_KP4;
  keymap[KEYCODE_SOFT_RIGHT] = SDLK_KP6;
  keymap[KEYCODE_ENTER] = SDLK_KP_ENTER;

  keymap[KEYCODE_VOLUME_UP] = SDLK_PAGEUP;
  keymap[KEYCODE_VOLUME_DOWN] = SDLK_PAGEDOWN;
  keymap[KEYCODE_SEARCH] = SDLK_END;
  keymap[KEYCODE_HOME] = SDLK_HOME;

  keymap[KEYCODE_CLEAR] = SDLK_CLEAR;
  keymap[KEYCODE_A] = SDLK_a;
  keymap[KEYCODE_B] = SDLK_b;
  keymap[KEYCODE_C] = SDLK_c;
  keymap[KEYCODE_D] = SDLK_d;
  keymap[KEYCODE_E] = SDLK_e;
  keymap[KEYCODE_F] = SDLK_f;
  keymap[KEYCODE_G] = SDLK_g;
  keymap[KEYCODE_H] = SDLK_h;
  keymap[KEYCODE_I] = SDLK_i;
  keymap[KEYCODE_J] = SDLK_j;
  keymap[KEYCODE_K] = SDLK_k;
  keymap[KEYCODE_L] = SDLK_l;
  keymap[KEYCODE_M] = SDLK_m;
  keymap[KEYCODE_N] = SDLK_n;
  keymap[KEYCODE_O] = SDLK_o;
  keymap[KEYCODE_P] = SDLK_p;
  keymap[KEYCODE_Q] = SDLK_q;
  keymap[KEYCODE_R] = SDLK_r;
  keymap[KEYCODE_S] = SDLK_s;
  keymap[KEYCODE_T] = SDLK_t;
  keymap[KEYCODE_U] = SDLK_u;
  keymap[KEYCODE_V] = SDLK_v;
  keymap[KEYCODE_W] = SDLK_w;
  keymap[KEYCODE_X] = SDLK_x;
  keymap[KEYCODE_Y] = SDLK_y;
  keymap[KEYCODE_Z] = SDLK_z;
  keymap[KEYCODE_COMMA] = SDLK_COMMA;
  keymap[KEYCODE_PERIOD] = SDLK_PERIOD;
  keymap[KEYCODE_TAB] = SDLK_TAB;
  keymap[KEYCODE_SPACE] = SDLK_SPACE;
  keymap[KEYCODE_DEL] = SDLK_DELETE;
  keymap[KEYCODE_GRAVE] = SDLK_BACKQUOTE;
  keymap[KEYCODE_MINUS] = SDLK_MINUS;
  keymap[KEYCODE_EQUALS] = SDLK_EQUALS;
  keymap[KEYCODE_LEFT_BRACKET] = SDLK_LEFTBRACKET;
  keymap[KEYCODE_RIGHT_BRACKET] = SDLK_RIGHTBRACKET;
  keymap[KEYCODE_BACKSLASH] = SDLK_BACKSLASH;
  keymap[KEYCODE_SEMICOLON] = SDLK_SEMICOLON;
  keymap[KEYCODE_APOSTROPHE] = SDLK_QUOTE;
  keymap[KEYCODE_SLASH] = SDLK_SLASH;
  keymap[KEYCODE_AT] = SDLK_AT;

  keymap[KEYCODE_PLUS] = SDLK_PLUS;

  /*

  keymap[KEYCODE_SYM] = SDLK_SYM;
  keymap[KEYCODE_NUM] = SDLK_NUM;

  keymap[KEYCODE_SOFT_LEFT] = SDLK_SOFT_LEFT;
  keymap[KEYCODE_SOFT_RIGHT] = SDLK_SOFT_RIGHT;

  keymap[KEYCODE_ALT_LEFT] = SDLK_ALT_LEFT;
  keymap[KEYCODE_ALT_RIGHT] = SDLK_ALT_RIGHT;
  keymap[KEYCODE_SHIFT_LEFT] = SDLK_SHIFT_LEFT;
  keymap[KEYCODE_SHIFT_RIGHT] = SDLK_SHIFT_RIGHT;

  keymap[KEYCODE_EXPLORER] = SDLK_EXPLORER;
  keymap[KEYCODE_ENVELOPE] = SDLK_ENVELOPE;
  keymap[KEYCODE_HEADSETHOOK] = SDLK_HEADSETHOOK;
  keymap[KEYCODE_FOCUS] = SDLK_FOCUS;
  keymap[KEYCODE_NOTIFICATION] = SDLK_NOTIFICATION;
  keymap[KEYCODE_MEDIA_PLAY_PAUSE=] = SDLK_MEDIA_PLAY_PAUSE=;
  keymap[KEYCODE_MEDIA_STOP] = SDLK_MEDIA_STOP;
  keymap[KEYCODE_MEDIA_NEXT] = SDLK_MEDIA_NEXT;
  keymap[KEYCODE_MEDIA_PREVIOUS] = SDLK_MEDIA_PREVIOUS;
  keymap[KEYCODE_MEDIA_REWIND] = SDLK_MEDIA_REWIND;
  keymap[KEYCODE_MEDIA_FAST_FORWARD] = SDLK_MEDIA_FAST_FORWARD;
  keymap[KEYCODE_MUTE] = SDLK_MUTE;
  */

}

static int AndroidTrackballKeyDelays[4] = {0,0,0,0};

// Key = -1 if we want to send KeyUp events from main loop
int processAndroidTrackballKeyDelays( int key, int action )
{
	#if ! defined(SDL_TRACKBALL_KEYUP_DELAY) || (SDL_TRACKBALL_KEYUP_DELAY == 0)
	return 0;
	#else
	// Send Directional Pad Up events with a delay, so app wil lthink we're holding the key a bit
	static const int KeysMapping[4] = {KEYCODE_DPAD_UP, KEYCODE_DPAD_DOWN, KEYCODE_DPAD_LEFT, KEYCODE_DPAD_RIGHT};
	int idx, idx2;
	SDL_keysym keysym;
	
	if( key < 0 )
	{
		for( idx = 0; idx < 4; idx ++ )
		{
			if( AndroidTrackballKeyDelays[idx] > 0 )
			{
				AndroidTrackballKeyDelays[idx] --;
				if( AndroidTrackballKeyDelays[idx] == 0 )
					SDL_PrivateKeyboard( SDL_RELEASED, TranslateKey(KeysMapping[idx], &keysym) );
			}
		}
	}
	else
	{
		idx = -1;
		// Too lazy to do switch or function
		if( key == KEYCODE_DPAD_UP )
			idx = 0;
		else if( key == KEYCODE_DPAD_DOWN )
			idx = 1;
		else if( key == KEYCODE_DPAD_LEFT )
			idx = 2;
		else if( key == KEYCODE_DPAD_RIGHT )
			idx = 3;
		if( idx >= 0 )
		{
			if( action && AndroidTrackballKeyDelays[idx] == 0 )
			{
				// User pressed key for the first time
				idx2 = (idx + 2) % 4; // Opposite key for current key - if it's still pressing, release it
				if( AndroidTrackballKeyDelays[idx2] > 0 )
				{
					AndroidTrackballKeyDelays[idx2] = 0;
					SDL_PrivateKeyboard( SDL_RELEASED, TranslateKey(KeysMapping[idx2], &keysym) );
				}
				SDL_PrivateKeyboard( SDL_PRESSED, TranslateKey(key, &keysym) );
			}
			else if( !action && AndroidTrackballKeyDelays[idx] == 0 )
			{
				// User released key - make a delay, do not send release event
				AndroidTrackballKeyDelays[idx] = SDL_TRACKBALL_KEYUP_DELAY;
			}
			else if( action && AndroidTrackballKeyDelays[idx] > 0 )
			{
				// User pressed key another time - add some more time for key to be pressed
				AndroidTrackballKeyDelays[idx] += SDL_TRACKBALL_KEYUP_DELAY;
				if( AndroidTrackballKeyDelays[idx] < SDL_TRACKBALL_KEYUP_DELAY * 4 )
					AndroidTrackballKeyDelays[idx] = SDL_TRACKBALL_KEYUP_DELAY * 4;
			}
			return 1;
		}
	}
	return 0;
	
	#endif
}