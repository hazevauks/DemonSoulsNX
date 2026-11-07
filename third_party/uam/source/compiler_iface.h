#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_dump.h"

#include "codegen/nv50_ir_driver.h"

#include "glsl_frontend.h"

#include "nv_attributes.h"
#include "nv_shader_header.h"
#include "dksh.h"

// DemonSoulsNX: the one class a program linking uam names; built as a library, everything else
// in it is hidden, so its Mesa copy cannot clash with another in the program (switch-mesa's EGL)
class __attribute__((visibility("default"))) DekoCompiler
{
	pipeline_stage m_stage;
	glsl_program m_glsl;
	const struct tgsi_token* m_tgsi;
	unsigned int m_tgsiNumTokens;
	nv50_ir_prog_info m_info;
	void* m_code;
	uint32_t m_codeSize;
	void* m_data;
	uint32_t m_dataSize;

	NvShaderHeader m_nvsh;
	DkshProgramHeader m_dkph;

	void RetrieveAndPadCode();
	void GenerateHeaders();

public:
	DekoCompiler(pipeline_stage stage, int optLevel = 3);
	~DekoCompiler();

	bool CompileGlsl(const char* glsl);
	void OutputDksh(const char* dkshFile);
	// DemonSoulsNX: the same bytes into memory (malloc'd, for free()), for uam linked into a program
	bool OutputDkshToMemory(void** data, size_t* size);
	void WriteDksh(FILE* f);
	void OutputRawCode(const char* rawFile);
	void OutputTgsi(const char* tgsiFile);
};
