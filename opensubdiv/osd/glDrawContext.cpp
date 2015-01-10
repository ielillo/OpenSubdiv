//
//   Copyright 2013 Pixar
//
//   Licensed under the Apache License, Version 2.0 (the "Apache License")
//   with the following modification; you may not use this file except in
//   compliance with the Apache License and the following modification to it:
//   Section 6. Trademarks. is deleted and replaced with:
//
//   6. Trademarks. This License does not grant permission to use the trade
//      names, trademarks, service marks, or product names of the Licensor
//      and its affiliates, except as required to comply with Section 4(c) of
//      the License and to reproduce the content of the NOTICE file.
//
//   You may obtain a copy of the Apache License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the Apache License with the above modification is
//   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
//   KIND, either express or implied. See the Apache License for the specific
//   language governing permissions and limitations under the Apache License.
//

#include "../osd/glDrawRegistry.h"
#include "../osd/glDrawContext.h"

#include "../osd/opengl.h"

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Osd {

GLDrawContext::GLDrawContext() :
    _patchIndexBuffer(0), _patchParamTextureBuffer(0), _fvarDataTextureBuffer(0),
    _vertexTextureBuffer(0), _vertexValenceTextureBuffer(0), _quadOffsetsTextureBuffer(0)
{
}

GLDrawContext::~GLDrawContext()
{
    glDeleteBuffers(1, &_patchIndexBuffer);
    glDeleteTextures(1, &_vertexTextureBuffer);
    glDeleteTextures(1, &_vertexValenceTextureBuffer);
    glDeleteTextures(1, &_quadOffsetsTextureBuffer);
    glDeleteTextures(1, &_patchParamTextureBuffer);
    glDeleteTextures(1, &_fvarDataTextureBuffer);
}

bool
GLDrawContext::SupportsAdaptiveTessellation()
{
#ifdef OSD_USES_GLEW
    // XXX: uncomment here to try tessellation on OSX
    // if (GLEW_ARB_tessellation_shader)
    //    return true;
#endif
    static const GLubyte *version = glGetString(GL_VERSION);
    if (version and version[0] == '4')
        return true;

    return false;
}

template <typename T> static GLuint
createTextureBuffer(T const &data, GLint format, int offset=0)
{
    GLuint buffer = 0, texture = 0;

#if defined(GL_ARB_texture_buffer_object) || defined(GL_VERSION_3_1)
    glGenTextures(1, &texture);
    glGenBuffers(1, &buffer);

#if defined(GL_EXT_direct_state_access)
    if (glNamedBufferDataEXT and glTextureBufferEXT) {
        glNamedBufferDataEXT(buffer, (data.size()-offset) * sizeof(typename T::value_type),
                             &data[offset], GL_STATIC_DRAW);
        glTextureBufferEXT(texture, GL_TEXTURE_BUFFER, format, buffer);
    } else {
#else
    {
#endif
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, (data.size()-offset) * sizeof(typename T::value_type),
                     &data[offset], GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glBindTexture(GL_TEXTURE_BUFFER, texture);
        glTexBuffer(GL_TEXTURE_BUFFER, format, buffer);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
    }
    glDeleteBuffers(1, &buffer);

#endif

    return texture;
}

GLDrawContext *
GLDrawContext::Create(Far::PatchTables const * patchTables, int numVertexElements) {

    if (patchTables) {

        GLDrawContext * result = new GLDrawContext();

        if (result->create(*patchTables, numVertexElements)) {
            return result;
        } else {
            delete result;
        }
    }
    return NULL;
}

bool
GLDrawContext::create(Far::PatchTables const & patchTables, int numVertexElements) {

    _isAdaptive = patchTables.IsFeatureAdaptive();

    // Process PTable
    Far::PatchTables::PatchVertsTable const & ptables = patchTables.GetPatchControlVerticesTable();

    glGenBuffers(1, &_patchIndexBuffer);

#if defined(GL_EXT_direct_state_access)
    if (glNamedBufferDataEXT) {
        glNamedBufferDataEXT(_patchIndexBuffer,
                             ptables.size() * sizeof(unsigned int), &ptables[0], GL_STATIC_DRAW);
    } else {
#else
    {
#endif
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _patchIndexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     ptables.size() * sizeof(unsigned int), &ptables[0], GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    DrawContext::ConvertPatchArrays(patchTables, _patchArrays,
        patchTables.GetMaxValence(), numVertexElements);

    // allocate and initialize additional buffer data

#if defined(GL_ARB_texture_buffer_object) || defined(GL_VERSION_3_1)
    // create vertex valence buffer and vertex texture
    Far::PatchTables::VertexValenceTable const &
        valenceTable = patchTables.GetVertexValenceTable();

    if (not valenceTable.empty()) {
        _vertexValenceTextureBuffer = createTextureBuffer(valenceTable, GL_R32I);

        // also create vertex texture buffer (will be updated in UpdateVertexTexture())
        glGenTextures(1, &_vertexTextureBuffer);
    }


    // create quad offset table buffer
    Far::PatchTables::QuadOffsetsTable const &
        quadOffsetTable = patchTables.GetQuadOffsetsTable();

    if (not quadOffsetTable.empty())
        _quadOffsetsTextureBuffer = createTextureBuffer(quadOffsetTable, GL_R32I);


    // create ptex coordinate buffer
    Far::PatchParamTable const &
        patchParamTables = patchTables.GetPatchParamTable();

    if (not patchParamTables.empty()) {
        std::vector<int> const &sharpnessIndexTable = patchTables.GetSharpnessIndexTable();
        if (sharpnessIndexTable.empty()) {
            _patchParamTextureBuffer = createTextureBuffer(patchParamTables, GL_RG32I);
        } else {
            // if indexed sharpnesses exists, flatten them and interleave into 3-component buffer
            std::vector<float> const &sharpnessValues = patchTables.GetSharpnessValues();
            size_t nPatches = patchParamTables.size();
            // PatchParam = sizeof(int)*2, 1 float for sharpness
            std::vector<unsigned int> buffer(nPatches * 3);

            for (size_t i = 0; i < nPatches; ++i) {
                float sharpness = sharpnessIndexTable[i] >=0 ?
                    sharpnessValues[sharpnessIndexTable[i]] : 0.0f;
                buffer[i*3+0] = patchParamTables[i].faceIndex;
                buffer[i*3+1] = patchParamTables[i].bitField.field;
                buffer[i*3+2] = *((unsigned int *)&sharpness);
            }

            _patchParamTextureBuffer = createTextureBuffer(buffer, GL_RGB32I);
        }
    }

    glBindBuffer(GL_TEXTURE_BUFFER, 0);
#endif

    return true;
}

#if defined(GL_ARB_texture_buffer_object) || defined(GL_VERSION_3_1)
bool
GLDrawContext::SetFVarDataTexture(
    Far::PatchTables const & patchTables, int fvarWidth, FVarData const & fvarData) {

    if (not fvarData.empty()) {

        FVarData fvarDataTable;

        packFVarData(patchTables, fvarWidth, fvarData, fvarDataTable);

        _fvarDataTextureBuffer = createTextureBuffer(fvarDataTable, GL_R32F);

        return true;
    }
    return false;
}
#else
bool
GLDrawContext::SetFVarDataTexture(
    Far::PatchTables const &, int, FVarData const &) {

    return false;
}
#endif

#if defined(GL_ARB_texture_buffer_object) || defined(GL_VERSION_3_1)
void
GLDrawContext::updateVertexTexture(GLuint vbo)
{

#if defined(GL_EXT_direct_state_access)
    if (glTextureBufferEXT) {
        glTextureBufferEXT(_vertexTextureBuffer, GL_TEXTURE_BUFFER, GL_R32F, vbo);
    } else {
#else
    {
#endif
        glBindTexture(GL_TEXTURE_BUFFER, _vertexTextureBuffer);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, vbo);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
    }
}
#else
void
GLDrawContext::updateVertexTexture(GLuint)
{
}
#endif


}  // end namespace Osd

}  // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv
