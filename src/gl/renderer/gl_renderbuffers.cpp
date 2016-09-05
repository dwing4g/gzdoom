/*
** gl_renderbuffers.cpp
** Render buffers used during rendering
**
**---------------------------------------------------------------------------
** Copyright 2016 Magnus Norddahl
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
** 4. When not used as part of GZDoom or a GZDoom derivative, this code will be
**    covered by the terms of the GNU Lesser General Public License as published
**    by the Free Software Foundation; either version 2.1 of the License, or (at
**    your option) any later version.
** 5. Full disclosure of the entire project's source code, except for third
**    party libraries is mandatory. (NOTE: This clause is non-negotiable!)
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
*/

#include "gl/system/gl_system.h"
#include "files.h"
#include "m_swap.h"
#include "v_video.h"
#include "gl/gl_functions.h"
#include "vectors.h"
#include "gl/system/gl_interface.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/system/gl_cvars.h"
#include "gl/system/gl_debug.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_renderbuffers.h"
#include "w_wad.h"
#include "i_system.h"
#include "doomerrors.h"

CVAR(Int, gl_multisample, 1, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CVAR(Bool, gl_renderbuffers, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)

//==========================================================================
//
// Initialize render buffers and textures used in rendering passes
//
//==========================================================================

FGLRenderBuffers::FGLRenderBuffers()
{
	for (int i = 0; i < NumPipelineTextures; i++)
	{
		mPipelineTexture[i] = 0;
		mPipelineFB[i] = 0;
	}

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&mOutputFB);
	glGetIntegerv(GL_MAX_SAMPLES, &mMaxSamples);
}

//==========================================================================
//
// Free render buffer resources
//
//==========================================================================

FGLRenderBuffers::~FGLRenderBuffers()
{
	ClearScene();
	ClearPipeline();
	ClearBloom();
}

void FGLRenderBuffers::ClearScene()
{
	DeleteFrameBuffer(mSceneFB);
	DeleteRenderBuffer(mSceneMultisample);
	DeleteRenderBuffer(mSceneDepthStencil);
	DeleteRenderBuffer(mSceneDepth);
	DeleteRenderBuffer(mSceneStencil);
}

void FGLRenderBuffers::ClearPipeline()
{
	for (int i = 0; i < NumPipelineTextures; i++)
	{
		DeleteFrameBuffer(mPipelineFB[i]);
		DeleteTexture(mPipelineTexture[i]);
	}
}

void FGLRenderBuffers::ClearBloom()
{
	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &level = BloomLevels[i];
		DeleteFrameBuffer(level.HFramebuffer);
		DeleteFrameBuffer(level.VFramebuffer);
		DeleteTexture(level.HTexture);
		DeleteTexture(level.VTexture);
		level = FGLBloomTextureLevel();
	}
}

void FGLRenderBuffers::DeleteTexture(GLuint &handle)
{
	if (handle != 0)
		glDeleteTextures(1, &handle);
	handle = 0;
}

void FGLRenderBuffers::DeleteRenderBuffer(GLuint &handle)
{
	if (handle != 0)
		glDeleteRenderbuffers(1, &handle);
	handle = 0;
}

void FGLRenderBuffers::DeleteFrameBuffer(GLuint &handle)
{
	if (handle != 0)
		glDeleteFramebuffers(1, &handle);
	handle = 0;
}

//==========================================================================
//
// Makes sure all render buffers have sizes suitable for rending at the
// specified resolution
//
//==========================================================================

bool FGLRenderBuffers::Setup(int width, int height, int sceneWidth, int sceneHeight)
{
	if (gl_renderbuffers != BuffersActive)
	{
		if (BuffersActive)
			glBindFramebuffer(GL_FRAMEBUFFER, mOutputFB);
		BuffersActive = gl_renderbuffers;
		GLRenderer->mShaderManager->ResetFixedColormap();
	}

	if (!IsEnabled())
		return false;
		
	if (width <= 0 || height <= 0)
		I_FatalError("Requested invalid render buffer sizes: screen = %dx%d", width, height);

	int samples = clamp((int)gl_multisample, 0, mMaxSamples);

	GLint activeTex;
	GLint textureBinding;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);

	if (width == mWidth && height == mHeight && mSamples != samples)
	{
		CreateScene(mWidth, mHeight, samples);
		mSamples = samples;
	}
	else if (width != mWidth || height != mHeight)
	{
		CreatePipeline(width, height);
		CreateScene(width, height, samples);
		mWidth = width;
		mHeight = height;
		mSamples = samples;
	}

	// Bloom bluring buffers need to match the scene to avoid bloom bleeding artifacts
	if (mBloomWidth != sceneWidth || mBloomHeight != sceneHeight)
	{
		CreateBloom(sceneWidth, sceneHeight);
		mBloomWidth = sceneWidth;
		mBloomHeight = sceneHeight;
	}

	glBindTexture(GL_TEXTURE_2D, textureBinding);
	glActiveTexture(activeTex);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (FailedCreate)
	{
		ClearScene();
		ClearPipeline();
		ClearBloom();
		mWidth = 0;
		mHeight = 0;
		mSamples = 0;
		mBloomWidth = 0;
		mBloomHeight = 0;
	}

	return !FailedCreate;
}

//==========================================================================
//
// Creates the scene buffers
//
//==========================================================================

void FGLRenderBuffers::CreateScene(int width, int height, int samples)
{
	ClearScene();

	if (samples > 1)
		mSceneMultisample = CreateRenderBuffer("SceneMultisample", GL_RGBA16F, samples, width, height);

	mSceneDepthStencil = CreateRenderBuffer("SceneDepthStencil", GL_DEPTH24_STENCIL8, samples, width, height);
	mSceneFB = CreateFrameBuffer("SceneFB", samples > 1 ? mSceneMultisample : mPipelineTexture[0], mSceneDepthStencil, samples > 1);
}

//==========================================================================
//
// Creates the buffers needed for post processing steps
//
//==========================================================================

void FGLRenderBuffers::CreatePipeline(int width, int height)
{
	ClearPipeline();

	for (int i = 0; i < NumPipelineTextures; i++)
	{
		mPipelineTexture[i] = Create2DTexture("PipelineTexture", GL_RGBA16F, width, height);
		mPipelineFB[i] = CreateFrameBuffer("PipelineFB", mPipelineTexture[i]);
	}
}

//==========================================================================
//
// Creates bloom pass working buffers
//
//==========================================================================

void FGLRenderBuffers::CreateBloom(int width, int height)
{
	ClearBloom();
	
	// No scene, no bloom!
	if (width <= 0 || height <= 0)
		return;

	int bloomWidth = MAX(width / 2, 1);
	int bloomHeight = MAX(height / 2, 1);
	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &level = BloomLevels[i];
		level.Width = MAX(bloomWidth / 2, 1);
		level.Height = MAX(bloomHeight / 2, 1);

		level.VTexture = Create2DTexture("Bloom.VTexture", GL_RGBA16F, level.Width, level.Height);
		level.HTexture = Create2DTexture("Bloom.HTexture", GL_RGBA16F, level.Width, level.Height);
		level.VFramebuffer = CreateFrameBuffer("Bloom.VFramebuffer", level.VTexture);
		level.HFramebuffer = CreateFrameBuffer("Bloom.HFramebuffer", level.HTexture);

		bloomWidth = level.Width;
		bloomHeight = level.Height;
	}
}

//==========================================================================
//
// Creates a 2D texture defaulting to linear filtering and clamp to edge
//
//==========================================================================

GLuint FGLRenderBuffers::Create2DTexture(const FString &name, GLuint format, int width, int height)
{
	GLuint type = (format == GL_RGBA16F) ? GL_FLOAT : GL_UNSIGNED_BYTE;
	GLuint handle = 0;
	glGenTextures(1, &handle);
	glBindTexture(GL_TEXTURE_2D, handle);
	FGLDebug::LabelObject(GL_TEXTURE, handle, name);
	glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return handle;
}

//==========================================================================
//
// Creates a render buffer
//
//==========================================================================

GLuint FGLRenderBuffers::CreateRenderBuffer(const FString &name, GLuint format, int width, int height)
{
	GLuint handle = 0;
	glGenRenderbuffers(1, &handle);
	glBindRenderbuffer(GL_RENDERBUFFER, handle);
	FGLDebug::LabelObject(GL_RENDERBUFFER, handle, name);
	glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
	return handle;
}

GLuint FGLRenderBuffers::CreateRenderBuffer(const FString &name, GLuint format, int samples, int width, int height)
{
	if (samples <= 1)
		return CreateRenderBuffer(name, format, width, height);

	GLuint handle = 0;
	glGenRenderbuffers(1, &handle);
	glBindRenderbuffer(GL_RENDERBUFFER, handle);
	FGLDebug::LabelObject(GL_RENDERBUFFER, handle, name);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, width, height);
	return handle;
}

//==========================================================================
//
// Creates a frame buffer
//
//==========================================================================

GLuint FGLRenderBuffers::CreateFrameBuffer(const FString &name, GLuint colorbuffer)
{
	GLuint handle = 0;
	glGenFramebuffers(1, &handle);
	glBindFramebuffer(GL_FRAMEBUFFER, handle);
	FGLDebug::LabelObject(GL_FRAMEBUFFER, handle, name);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer, 0);
	if (CheckFrameBufferCompleteness())
		ClearFrameBuffer(false, false);
	return handle;
}

GLuint FGLRenderBuffers::CreateFrameBuffer(const FString &name, GLuint colorbuffer, GLuint depthstencil, bool colorIsARenderBuffer)
{
	GLuint handle = 0;
	glGenFramebuffers(1, &handle);
	glBindFramebuffer(GL_FRAMEBUFFER, handle);
	FGLDebug::LabelObject(GL_FRAMEBUFFER, handle, name);
	if (colorIsARenderBuffer)
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorbuffer);
	else
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthstencil);
	if (CheckFrameBufferCompleteness())
		ClearFrameBuffer(true, true);
	return handle;
}

GLuint FGLRenderBuffers::CreateFrameBuffer(const FString &name, GLuint colorbuffer, GLuint depth, GLuint stencil, bool colorIsARenderBuffer)
{
	GLuint handle = 0;
	glGenFramebuffers(1, &handle);
	glBindFramebuffer(GL_FRAMEBUFFER, handle);
	FGLDebug::LabelObject(GL_FRAMEBUFFER, handle, name);
	if (colorIsARenderBuffer)
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorbuffer);
	else
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
	if (CheckFrameBufferCompleteness())
		ClearFrameBuffer(true, true);
	return handle;
}

//==========================================================================
//
// Verifies that the frame buffer setup is valid
//
//==========================================================================

bool FGLRenderBuffers::CheckFrameBufferCompleteness()
{
	GLenum result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (result == GL_FRAMEBUFFER_COMPLETE)
		return true;

	FailedCreate = true;

#if 0
	FString error = "glCheckFramebufferStatus failed: ";
	switch (result)
	{
	default: error.AppendFormat("error code %d", (int)result); break;
	case GL_FRAMEBUFFER_UNDEFINED: error << "GL_FRAMEBUFFER_UNDEFINED"; break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: error << "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"; break;
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: error << "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"; break;
	case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: error << "GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER"; break;
	case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: error << "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER"; break;
	case GL_FRAMEBUFFER_UNSUPPORTED: error << "GL_FRAMEBUFFER_UNSUPPORTED"; break;
	case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: error << "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE"; break;
	case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: error << "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS"; break;
	}
	I_FatalError(error);
#endif

	return false;
}

//==========================================================================
//
// Clear frame buffer to make sure it never contains uninitialized data
//
//==========================================================================

void FGLRenderBuffers::ClearFrameBuffer(bool stencil, bool depth)
{
	GLboolean scissorEnabled;
	GLint stencilValue;
	GLdouble depthValue;
	glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);
	glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencilValue);
	glGetDoublev(GL_DEPTH_CLEAR_VALUE, &depthValue);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0, 0.0, 0.0, 0.0);
	glClearDepth(0.0);
	glClearStencil(0);
	GLenum flags = GL_COLOR_BUFFER_BIT;
	if (stencil)
		flags |= GL_STENCIL_BUFFER_BIT;
	if (depth)
		flags |= GL_DEPTH_BUFFER_BIT;
	glClear(flags);
	glClearStencil(stencilValue);
	glClearDepth(depthValue);
	if (scissorEnabled)
		glEnable(GL_SCISSOR_TEST);
}

//==========================================================================
//
// Resolves the multisample frame buffer by copying it to the scene texture
//
//==========================================================================

void FGLRenderBuffers::BlitSceneToTexture()
{
	mCurrentPipelineTexture = 0;

	if (mSamples <= 1)
		return;

	glBindFramebuffer(GL_READ_FRAMEBUFFER, mSceneFB);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mPipelineFB[mCurrentPipelineTexture]);
	glBlitFramebuffer(0, 0, mWidth, mHeight, 0, 0, mWidth, mHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	if ((gl.flags & RFL_INVALIDATE_BUFFER) != 0)
	{
		GLenum attachments[2] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT };
		glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 2, attachments);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

//==========================================================================
//
// Makes the scene frame buffer active (multisample, depth, stecil, etc.)
//
//==========================================================================

void FGLRenderBuffers::BindSceneFB()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mSceneFB);
}

//==========================================================================
//
// Binds the current scene/effect/hud texture to the specified texture unit
//
//==========================================================================

void FGLRenderBuffers::BindCurrentTexture(int index)
{
	glActiveTexture(GL_TEXTURE0 + index);
	glBindTexture(GL_TEXTURE_2D, mPipelineTexture[mCurrentPipelineTexture]);
}

//==========================================================================
//
// Makes the frame buffer for the current texture active 
//
//==========================================================================

void FGLRenderBuffers::BindCurrentFB()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mPipelineFB[mCurrentPipelineTexture]);
}

//==========================================================================
//
// Makes the frame buffer for the next texture active
//
//==========================================================================

void FGLRenderBuffers::BindNextFB()
{
	int out = (mCurrentPipelineTexture + 1) % NumPipelineTextures;
	glBindFramebuffer(GL_FRAMEBUFFER, mPipelineFB[out]);
}

//==========================================================================
//
// Next pipeline texture now contains the output
//
//==========================================================================

void FGLRenderBuffers::NextTexture()
{
	mCurrentPipelineTexture = (mCurrentPipelineTexture + 1) % NumPipelineTextures;
}

//==========================================================================
//
// Makes the screen frame buffer active
//
//==========================================================================

void FGLRenderBuffers::BindOutputFB()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mOutputFB);
}

//==========================================================================
//
// Returns true if render buffers are supported and should be used
//
//==========================================================================

bool FGLRenderBuffers::IsEnabled()
{
	return BuffersActive && !gl.legacyMode && !FailedCreate;
}

bool FGLRenderBuffers::FailedCreate = false;
bool FGLRenderBuffers::BuffersActive = false;
