﻿/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2017, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
copyright notice, this list of conditions and the
following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other
materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
contributors may be used to endorse or promote products
derived from this software without specific prior
written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/
#ifndef ASSIMP_BUILD_NO_EXPORT
#ifndef ASSIMP_BUILD_NO_FBX_EXPORTER

#include "FBXExporter.h"

#include "StreamWriter.h"

#include "Exceptional.h" // DeadlyExportError
//#include "StringComparison.h"
//#include "ByteSwapper.h"

//#include "SplitLargeMeshes.h"

//#include <assimp/SceneCombiner.h>
#include <assimp/version.h>
#include <assimp/IOSystem.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/DefaultLogger.hpp>
//#include <assimp/material.h>
#include <assimp/scene.h>
#include <assimp/mesh.h>

// Header files, standard library.
#include <memory> // shared_ptr
#include <string>
#include <sstream> // stringstream
#include <ctime> // localtime, tm_*
#include <map>
#include <unordered_set>
#include <iostream> // endl
using std::cout; using std::endl;

#include "debug_extra.cpp" // temporary

// RESOURCES:
// https://code.blender.org/2013/08/fbx-binary-file-format-specification/
// https://wiki.blender.org/index.php/User:Mont29/Foundation/FBX_File_Structure

constexpr double DEG = 360.0 / 6.283185307179586476925286766559;

// some constants that we'll use for writing metadata
namespace FBX {
    const std::string EXPORT_VERSION_STR = "7.4.0";
    const uint32_t EXPORT_VERSION_INT = 7400; // 7.4 == 2014/2015
    // FBX files have some hashed values that depend on the creation time field,
    // but for now we don't actually know how to generate these.
    // what we can do is set them to a known-working version.
    // this is the data that Blender uses in their FBX export process.
    const std::string GENERIC_CTIME = "1970-01-01 10:00:00:000";
    const std::string GENERIC_FILEID = "\x28\xb3\x2a\xeb\xb6\x24\xcc\xc2\xbf\xc8\xb0\x2a\xa9\x2b\xfc\xf1";
    const std::string GENERIC_FOOTID = "\xfa\xbc\xab\x09\xd0\xc8\xd4\x66\xb1\x76\xfb\x83\x1c\xf7\x26\x7e";
}

using namespace Assimp;
using namespace FBX;

namespace Assimp {

    // ------------------------------------------------------------------------------------------------
    // Worker function for exporting a scene to binary FBX. Prototyped and registered in Exporter.cpp
    void ExportSceneFBX (
        const char* pFile,
        IOSystem* pIOSystem,
        const aiScene* pScene,
        const ExportProperties* pProperties
    ){
        // initialze the exporter
        FBXExporter exporter(pScene, pProperties);
        
        // perform binary export
        exporter.ExportBinary(pFile, pIOSystem);
    }

    // ------------------------------------------------------------------------------------------------
    // Worker function for exporting a scene to ASCII FBX. Prototyped and registered in Exporter.cpp
    void ExportSceneFBXA (
        const char* pFile,
        IOSystem* pIOSystem,
        const aiScene* pScene,
        const ExportProperties* pProperties
    ){
        // initialze the exporter
        FBXExporter exporter(pScene, pProperties);
        
        // perform ascii export
        exporter.ExportAscii(pFile, pIOSystem);
    }

} // end of namespace Assimp

FBXExporter::FBXExporter (
    const aiScene* pScene,
    const ExportProperties* pProperties
)
    : mScene(pScene)
    , mProperties(pProperties)
{
    // will probably need to determine UIDs, connections, etc here.
    // basically anything that needs to be known
    // before we start writing sections to the stream.
}

void FBXExporter::ExportBinary (
    const char* pFile,
    IOSystem* pIOSystem
){
    // remember that we're exporting in binary mode
    binary = true;
    
    // open the indicated file for writing (in binary mode)
    outfile.reset(pIOSystem->Open(pFile,"wb"));
    if (!outfile) {
        throw DeadlyExportError(
            "could not open output .fbx file: " + std::string(pFile)
        );
    }
    
    print_node_heirarchy(mScene->mRootNode, "", true);
    
    // first a binary-specific file header
    WriteBinaryHeader();
    
    // the rest of the file is in node entries.
    // we have to serialize each entry before we write to the output,
    // as the first thing we write is the byte offset of the _next_ entry.
    // Either that or we can skip back to write the offset when we finish.
    WriteAllNodes();
    
    // finally we have a binary footer to the file
    WriteBinaryFooter();
    
    // explicitly release file pointer,
    // so we don't have to rely on class destruction.
    outfile.reset();
}

void FBXExporter::ExportAscii (
    const char* pFile,
    IOSystem* pIOSystem
){
    // remember that we're exporting in ascii mode
    binary = false;
    
    // open the indicated file for writing in text mode
    outfile.reset(pIOSystem->Open(pFile,"wt"));
    if (!outfile) {
        throw DeadlyExportError(
            "could not open output .fbx file: " + std::string(pFile)
        );
    }
    
    // this isn't really necessary,
    // but the Autodesk FBX SDK puts a similar comment at the top of the file.
    // Theirs declares that the file copyright is owned by Autodesk...
    std::stringstream head;
    using std::endl;
    head << "; FBX " << EXPORT_VERSION_STR << " project file" << endl;
    head << "; Created by the Open Asset Import Library (Assimp)" << endl;
    head << "; http://assimp.org" << endl;
    head << "; -------------------------------------------------" << endl;
    head << endl;
    const std::string ascii_header = head.str();
    outfile->Write(ascii_header.c_str(), ascii_header.size(), 1);
    
    // write all the sections
    WriteAllNodes();
    
    // explicitly release file pointer,
    // so we don't have to rely on class destruction.
    outfile.reset();
}

void FBXExporter::WriteBinaryHeader()
{
    // first a specific sequence of 23 bytes, always the same
    const char binary_header[24] = "Kaydara FBX Binary\x20\x20\x00\x1a\x00";
    outfile->Write(binary_header, 1, 23);
    
    // then FBX version number, "multiplied" by 1000, as little-endian uint32.
    // so 7.3 becomes 7300 == 0x841C0000, 7.4 becomes 7400 == 0xE81C0000, etc
    {
        StreamWriterLE outstream(outfile);
        outstream.PutU4(EXPORT_VERSION_INT);
    } // StreamWriter destructor writes the data to the file
    
    // after this the node data starts immediately
    // (probably with the FBXHEaderExtension node)
}

void FBXExporter::WriteBinaryFooter()
{
    outfile->Write(NULL_RECORD.c_str(), NULL_RECORD.size(), 1);

    outfile->Write(GENERIC_FOOTID.c_str(), GENERIC_FOOTID.size(), 1);
    for (size_t i = 0; i < 4; ++i) {
        outfile->Write("\x00", 1, 1);
    }

    // here some padding is added for alignment to 16 bytes.
    // if already aligned, the full 16 bytes is added.
    size_t pos = outfile->Tell();
    size_t pad = 16 - (pos % 16);
    for (size_t i = 0; i < pad; ++i) {
        outfile->Write("\x00", 1, 1);
    }

    // now the file version again
    {
        StreamWriterLE outstream(outfile);
        outstream.PutU4(EXPORT_VERSION_INT);
    } // StreamWriter destructor writes the data to the file

    // and finally some binary footer added to all files
    for (size_t i = 0; i < 120; ++i) {
        outfile->Write("\x00", 1, 1);
    }
    outfile->Write(
        "\xf8\x5a\x8c\x6a\xde\xf5\xd9\x7e\xec\xe9\x0c\xe3\x75\x8f\x29\x0b",
        1,
        16
    );
}

void FBXExporter::WriteAllNodes ()
{
    // header
    // (and fileid, creation time, creator, if binary)
    WriteHeaderExtension();
    
    // global settings
    WriteGlobalSettings();
    
    // documents
    WriteDocuments();
    
    // references
    WriteReferences();
    
    // definitions
    WriteDefinitions();
    
    // objects
    WriteObjects();
    
    // connections
    WriteConnections();
    
    // WriteTakes? (deprecated since at least 2015 (fbx 7.4))
}

//FBXHeaderExtension top-level node
void FBXExporter::WriteHeaderExtension ()
{
    FBX::Node n("FBXHeaderExtension");
    StreamWriterLE outstream(outfile);
    
    // begin node
    n.Begin(outstream);
    
    // write properties
    // (none)
    
    // finish properties
    n.EndProperties(outstream, 0);
    
    // write child nodes
    WritePropertyNode("FBXHeaderVersion", int32_t(1003), outstream);
    WritePropertyNode("FBXVersion", int32_t(EXPORT_VERSION_INT), outstream);
    WritePropertyNode("EncryptionType", int32_t(0), outstream);
    
    FBX::Node CreationTimeStamp("CreationTimeStamp");
    time_t rawtime;
    time(&rawtime);
    struct tm * now = localtime(&rawtime);
    CreationTimeStamp.AddChild("Version", int32_t(1000));
    CreationTimeStamp.AddChild("Year", int32_t(now->tm_year + 1900));
    CreationTimeStamp.AddChild("Month", int32_t(now->tm_mon + 1));
    CreationTimeStamp.AddChild("Day", int32_t(now->tm_mday));
    CreationTimeStamp.AddChild("Hour", int32_t(now->tm_hour));
    CreationTimeStamp.AddChild("Minute", int32_t(now->tm_min));
    CreationTimeStamp.AddChild("Second", int32_t(now->tm_sec));
    CreationTimeStamp.AddChild("Millisecond", int32_t(0));
    CreationTimeStamp.Dump(outstream);
    
    std::stringstream creator;
    creator << "Open Asset Import Library (Assimp) " << aiGetVersionMajor()
            << "." << aiGetVersionMinor() << "." << aiGetVersionRevision();
    WritePropertyNode("Creator", creator.str(), outstream);
    
    FBX::Node sceneinfo("SceneInfo");
    //sceneinfo.AddProperty("GlobalInfo" + FBX::SEPARATOR + "SceneInfo");
    // not sure if any of this is actually needed,
    // so just write an empty node for now.
    sceneinfo.Dump(outstream);
    
    // finish node
    n.End(outstream, true);
    
    // that's it for FBXHeaderExtension...
    
    // but binary files also need top-level FileID, CreationTime, Creator:
    std::vector<uint8_t> raw(GENERIC_FILEID.size());
    for (size_t i = 0; i < GENERIC_FILEID.size(); ++i) {
        raw[i] = uint8_t(GENERIC_FILEID[i]);
    }
    WritePropertyNode("FileId", raw, outstream);
    WritePropertyNode("CreationTime", GENERIC_CTIME, outstream);
    WritePropertyNode("Creator", creator.str(), outstream);
}

void FBXExporter::WriteGlobalSettings ()
{
    FBX::Node gs("GlobalSettings");
    gs.AddChild("Version", int32_t(1000));

    FBX::Node p("Properties70");
    p.AddP70int("UpAxis", 1);
    p.AddP70int("UpAxisSign", 1);
    p.AddP70int("FrontAxis", 2);
    p.AddP70int("FrontAxisSign", 1);
    p.AddP70int("CoordAxis", 0);
    p.AddP70int("CoordAxisSign", 1);
    p.AddP70int("OriginalUpAxis", 1);
    p.AddP70int("OriginalUpAxisSign", 1);
    p.AddP70double("UnitScaleFactor", 1.0);
    p.AddP70double("OriginalUnitScaleFactor", 1.0);
    p.AddP70color("AmbientColor", 0.0, 0.0, 0.0);
    p.AddP70string("DefaultCamera", "Producer Perspective");
    p.AddP70enum("TimeMode", 11);
    p.AddP70enum("TimeProtocol", 2);
    p.AddP70enum("SnapOnFrameMode", 0);
    p.AddP70time("TimeSpanStart", 0); // ?
    p.AddP70time("TimeSpanStop", 46186158000); // ?
    p.AddP70double("CustomFrameRate", -1.0);
    p.AddP70("TimeMarker", "Compound", "", ""); // not sure what this is
    p.AddP70int("CurrentTimeMarker", -1);
    gs.AddChild(p);
    
    gs.Dump(outfile);
}

void FBXExporter::WriteDocuments ()
{
    // not sure what the use of multiple documents would be,
    // or whether any end-appication supports it
    FBX::Node docs("Documents");
    docs.AddChild("Count", int32_t(1));
    FBX::Node doc("Document");
    
    // generate uid
    int64_t uid = generate_uid();
    doc.AddProperties(uid, "", "Scene");
    FBX::Node p("Properties70");
    p.AddP70("SourceObject", "object", "", ""); // what is this even for?
    p.AddP70string("ActiveAnimStackName", ""); // should do this properly?
    doc.AddChild(p);
    
    // UID for root node in scene heirarchy.
    // always set to 0 in the case of a single document.
    // not sure what happens if more than one document exists.
    doc.AddChild("RootNode", int64_t(0));
    
    docs.AddChild(doc);
    docs.Dump(outfile);
}

void FBXExporter::WriteReferences ()
{
    // always empty for now.
    // not really sure what this is for.
    FBX::Node n("References");
    n.Dump(outfile);
}

size_t count_nodes(const aiNode* n) {
    size_t count = 1;
    for (size_t i = 0; i < n->mNumChildren; ++i) {
        count += count_nodes(n->mChildren[i]);
    }
    return count;
}

bool has_phong_mat(const aiScene* scene)
{
    // just search for any material with a shininess exponent
    for (size_t i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        float shininess = 0;
        mat->Get(AI_MATKEY_SHININESS, shininess);
        if (shininess > 0) {
            return true;
        }
    }
    return false;
}

size_t count_images(const aiScene* scene) {
    std::unordered_set<std::string> images;
    aiString texpath;
    for (size_t i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        for (
            size_t tt = aiTextureType_DIFFUSE;
            tt < aiTextureType_UNKNOWN;
            ++tt
        ){
            const aiTextureType textype = static_cast<aiTextureType>(tt);
            const size_t texcount = mat->GetTextureCount(textype);
            for (size_t j = 0; j < texcount; ++j) {
                mat->GetTexture(textype, j, &texpath);
                images.insert(std::string(texpath.C_Str()));
            }
        }
    }
    for (auto &s : images) {
        cout << "found image: " << s << endl;
    }
    return images.size();
}

size_t count_textures(const aiScene* scene) {
    size_t count = 0;
    for (size_t i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        for (
            size_t tt = aiTextureType_DIFFUSE;
            tt < aiTextureType_UNKNOWN;
            ++tt
        ){
            // TODO: handle layered textures
            if (mat->GetTextureCount(static_cast<aiTextureType>(tt)) > 0) {
                count += 1;
            }
        }
    }
    return count;
}

void FBXExporter::WriteDefinitions ()
{
    // basically this is just bookkeeping:
    // determining how many of each type of object there are
    // and specifying the base properties to use when otherwise unspecified.
    
    // we need to count the objects
    int32_t count;
    int32_t total_count = 0;
    
    // and store them
    std::vector<FBX::Node> object_nodes;
    FBX::Node n, pt, p;
    
    // GlobalSettings
    // this seems to always be here in Maya exports
    n = FBX::Node("ObjectType", Property("GlobalSettings"));
    count = 1;
    n.AddChild("Count", count);
    object_nodes.push_back(n);
    total_count += count;
    
    // AnimationStack / FbxAnimStack
    // this seems to always be here in Maya exports
    count = 0;
    if (count) {
        n = FBX::Node("ObjectType", Property("AnimationStack"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxAnimStack"));
        p = FBX::Node("Properties70");
        p.AddP70string("Description", "");
        p.AddP70time("LocalStart", 0);
        p.AddP70time("LocalStop", 0);
        p.AddP70time("ReferenceStart", 0);
        p.AddP70time("ReferenceStop", 0);
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // AnimationLayer / FbxAnimLayer
    // this seems to always be here in Maya exports
    count = 0;
    if (count) {
        n = FBX::Node("ObjectType", Property("AnimationLayer"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FBXAnimLayer"));
        p = FBX::Node("Properties70");
        p.AddP70("Weight", "Number", "", "A", double(100));
        p.AddP70bool("Mute", 0);
        p.AddP70bool("Solo", 0);
        p.AddP70bool("Lock", 0);
        p.AddP70color("Color", 0.8, 0.8, 0.8);
        p.AddP70("BlendMode", "enum", "", "", int32_t(0));
        p.AddP70("RotationAccumulationMode", "enum", "", "", int32_t(0));
        p.AddP70("ScaleAccumulationMode", "enum", "", "", int32_t(0));
        p.AddP70("BlendModeBypass", "ULongLong", "", "", int64_t(0));
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // NodeAttribute
    // this is completely absurd.
    // there can only be one "NodeAttribute" template,
    // but FbxSkeleton, FbxCamera, FbxLight all are "NodeAttributes".
    // so if only one exists we should set the template for that,
    // otherwise... we just pick one :/.
    // the others have to set all their properties every instance,
    // because there's no template.
    count = 0;
    if (count) {
        // FbxSkeleton
        n = FBX::Node("ObjectType", Property("NodeAttribute"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxSkeleton"));
        p = FBX::Node("Properties70");
        p.AddP70color("Color", 0.8, 0.8, 0.8);
        p.AddP70double("Size", 100);
        p.AddP70("LimbLength", "double", "Number", "H", double(1));
        // note: not sure what the "H" flag is for - hidden?
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // Model / FbxNode
    // <~~ node heirarchy
    count = count_nodes(mScene->mRootNode) - 1; // (not counting root node)
    if (count) {
        n = FBX::Node("ObjectType", Property("Model"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxNode"));
        p = FBX::Node("Properties70");
        p.AddP70enum("QuaternionInterpolate", 0);
        p.AddP70vector("RotationOffset", 0.0, 0.0, 0.0);
        p.AddP70vector("RotationPivot", 0.0, 0.0, 0.0);
        p.AddP70vector("ScalingOffset", 0.0, 0.0, 0.0);
        p.AddP70vector("ScalingPivot", 0.0, 0.0, 0.0);
        p.AddP70bool("TranslationActive", 0);
        p.AddP70vector("TranslationMin", 0.0, 0.0, 0.0);
        p.AddP70vector("TranslationMax", 0.0, 0.0, 0.0);
        p.AddP70bool("TranslationMinX", 0);
        p.AddP70bool("TranslationMinY", 0);
        p.AddP70bool("TranslationMinZ", 0);
        p.AddP70bool("TranslationMaxX", 0);
        p.AddP70bool("TranslationMaxY", 0);
        p.AddP70bool("TranslationMaxZ", 0);
        p.AddP70enum("RotationOrder", 0);
        p.AddP70bool("RotationSpaceForLimitOnly", 0);
        p.AddP70double("RotationStiffnessX", 0.0);
        p.AddP70double("RotationStiffnessY", 0.0);
        p.AddP70double("RotationStiffnessZ", 0.0);
        p.AddP70double("AxisLen", 10.0);
        p.AddP70vector("PreRotation", 0.0, 0.0, 0.0);
        p.AddP70vector("PostRotation", 0.0, 0.0, 0.0);
        p.AddP70bool("RotationActive", 0);
        p.AddP70vector("RotationMin", 0.0, 0.0, 0.0);
        p.AddP70vector("RotationMax", 0.0, 0.0, 0.0);
        p.AddP70bool("RotationMinX", 0);
        p.AddP70bool("RotationMinY", 0);
        p.AddP70bool("RotationMinZ", 0);
        p.AddP70bool("RotationMaxX", 0);
        p.AddP70bool("RotationMaxY", 0);
        p.AddP70bool("RotationMaxZ", 0);
        p.AddP70enum("InheritType", 0);
        p.AddP70bool("ScalingActive", 0);
        p.AddP70vector("ScalingMin", 0.0, 0.0, 0.0);
        p.AddP70vector("ScalingMax", 1.0, 1.0, 1.0);
        p.AddP70bool("ScalingMinX", 0);
        p.AddP70bool("ScalingMinY", 0);
        p.AddP70bool("ScalingMinZ", 0);
        p.AddP70bool("ScalingMaxX", 0);
        p.AddP70bool("ScalingMaxY", 0);
        p.AddP70bool("ScalingMaxZ", 0);
        p.AddP70vector("GeometricTranslation", 0.0, 0.0, 0.0);
        p.AddP70vector("GeometricRotation", 0.0, 0.0, 0.0);
        p.AddP70vector("GeometricScaling", 1.0, 1.0, 1.0);
        p.AddP70double("MinDampRangeX", 0.0);
        p.AddP70double("MinDampRangeY", 0.0);
        p.AddP70double("MinDampRangeZ", 0.0);
        p.AddP70double("MaxDampRangeX", 0.0);
        p.AddP70double("MaxDampRangeY", 0.0);
        p.AddP70double("MaxDampRangeZ", 0.0);
        p.AddP70double("MinDampStrengthX", 0.0);
        p.AddP70double("MinDampStrengthY", 0.0);
        p.AddP70double("MinDampStrengthZ", 0.0);
        p.AddP70double("MaxDampStrengthX", 0.0);
        p.AddP70double("MaxDampStrengthY", 0.0);
        p.AddP70double("MaxDampStrengthZ", 0.0);
        p.AddP70double("PreferedAngleX", 0.0);
        p.AddP70double("PreferedAngleY", 0.0);
        p.AddP70double("PreferedAngleZ", 0.0);
        p.AddP70("LookAtProperty", "object", "", "");
        p.AddP70("UpVectorProperty", "object", "", "");
        p.AddP70bool("Show", 1);
        p.AddP70bool("NegativePercentShapeSupport", 1);
        p.AddP70int("DefaultAttributeIndex", -1);
        p.AddP70bool("Freeze", 0);
        p.AddP70bool("LODBox", 0);
        p.AddP70("Lcl Translation", "Lcl Translation", "", "A", double(0), double(0), double(0));
        p.AddP70("Lcl Rotation", "Lcl Rotation", "", "A", double(0), double(0), double(0));
        p.AddP70("Lcl Scaling", "Lcl Scaling", "", "A", double(1), double(1), double(1));
        p.AddP70("Visibility", "Visibility", "", "A", double(1));
        p.AddP70("Visibility Inheritance", "Visibility Inheritance", "", "", int32_t(1));
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // Geometry / FbxMesh
    // <~~ aiMesh
    count = mScene->mNumMeshes;
    if (count) {
        n = FBX::Node("ObjectType", Property("Geometry"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxMesh"));
        p = FBX::Node("Properties70");
        p.AddP70color("Color", 0, 0, 0);
        p.AddP70vector("BBoxMin", 0, 0, 0);
        p.AddP70vector("BBoxMax", 0, 0, 0);
        p.AddP70bool("Primary Visibility", 1);
        p.AddP70bool("Casts Shadows", 1);
        p.AddP70bool("Receive Shadows", 1);
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // Material / FbxSurfacePhong, FbxSurfaceLambert, FbxSurfaceMaterial
    // <~~ aiMaterial
    // basically if there's any phong material this is defined as phong,
    // and otherwise lambert.
    // More complex materials cause a bare-bones FbxSurfaceMaterial definition
    // and are treated specially, as they're not really supported by FBX.
    // TODO: support Maya's Stingray PBS material
    count = mScene->mNumMaterials;
    if (count) {
        bool has_phong = has_phong_mat(mScene);
        n = FBX::Node("ObjectType", Property("Material"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate");
        if (has_phong) {
            pt.AddProperty("FbxSurfacePhong");
        } else {
            pt.AddProperty("FbxSurfaceLambert");
        }
        p = FBX::Node("Properties70");
        if (has_phong) {
            p.AddP70string("ShadingModel", "Phong");
        } else {
            p.AddP70string("ShadingModel", "Lambert");
        }
        p.AddP70bool("MultiLayer", 0);
        p.AddP70colorA("EmissiveColor", 0.0, 0.0, 0.0);
        p.AddP70numberA("EmissiveFactor", 1.0);
        p.AddP70colorA("AmbientColor", 0.2, 0.2, 0.2);
        p.AddP70numberA("AmbientFactor", 1.0);
        p.AddP70colorA("DiffuseColor", 0.8, 0.8, 0.8);
        p.AddP70numberA("DiffuseFactor", 1.0);
        p.AddP70vector("Bump", 0.0, 0.0, 0.0);
        p.AddP70vector("NormalMap", 0.0, 0.0, 0.0);
        p.AddP70double("BumpFactor", 1.0);
        p.AddP70colorA("TransparentColor", 0.0, 0.0, 0.0);
        p.AddP70numberA("TransparencyFactor", 0.0);
        p.AddP70color("DisplacementColor", 0.0, 0.0, 0.0);
        p.AddP70double("DisplacementFactor", 1.0);
        p.AddP70color("VectorDisplacementColor", 0.0, 0.0, 0.0);
        p.AddP70double("VectorDisplacementFactor", 1.0);
        if (has_phong) {
            p.AddP70colorA("SpecularColor", 0.2, 0.2, 0.2);
            p.AddP70numberA("SpecularFactor", 1.0);
            p.AddP70numberA("ShininessExponent", 20.0);
            p.AddP70colorA("ReflectionColor", 0.0, 0.0, 0.0);
            p.AddP70numberA("ReflectionFactor", 1.0);
        }
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // Video / FbxVideo
    // one for each image file.
    count = count_images(mScene);
    if (count) {
        n = FBX::Node("ObjectType", Property("Video"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxVideo"));
        p = FBX::Node("Properties70");
        p.AddP70bool("ImageSequence", 0);
        p.AddP70int("ImageSequenceOffset", 0);
        p.AddP70double("FrameRate", 0.0);
        p.AddP70int("LastFrame", 0);
        p.AddP70int("Width", 0);
        p.AddP70int("Height", 0);
        p.AddP70("Path", "KString", "XRefUrl", "", "");
        p.AddP70int("StartFrame", 0);
        p.AddP70int("StopFrame", 0);
        p.AddP70double("PlaySpeed", 0.0);
        p.AddP70time("Offset", 0);
        p.AddP70enum("InterlaceMode", 0);
        p.AddP70bool("FreeRunning", 0);
        p.AddP70bool("Loop", 0);
        p.AddP70enum("AccessMode", 0);
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // Texture / FbxFileTexture
    // <~~ aiTexture
    count = count_textures(mScene);
    if (count) {
        n = FBX::Node("ObjectType", Property("Texture"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxFileTexture"));
        p = FBX::Node("Properties70");
        p.AddP70enum("TextureTypeUse", 0);
        p.AddP70numberA("Texture alpha", 1.0);
        p.AddP70enum("CurrentMappingType", 0);
        p.AddP70enum("WrapModeU", 0);
        p.AddP70enum("WrapModeV", 0);
        p.AddP70bool("UVSwap", 0);
        p.AddP70bool("PremultiplyAlpha", 1);
        p.AddP70vectorA("Translation", 0.0, 0.0, 0.0);
        p.AddP70vectorA("Rotation", 0.0, 0.0, 0.0);
        p.AddP70vectorA("Scaling", 1.0, 1.0, 1.0);
        p.AddP70vector("TextureRotationPivot", 0.0, 0.0, 0.0);
        p.AddP70vector("TextureScalingPivot", 0.0, 0.0, 0.0);
        p.AddP70enum("CurrentTextureBlendMode", 1);
        p.AddP70string("UVSet", "default");
        p.AddP70bool("UseMaterial", 0);
        p.AddP70bool("UseMipMap", 0);
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // AnimationCurveNode / FbxAnimCurveNode
    count = 0;
    if (count) {
        n = FBX::Node("ObjectType", Property("AnimationCurveNode"));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property("FbxAnimCurveNode"));
        p = FBX::Node("Properties70");
        p.AddP70("d", "Compound", "", "");
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // CollectionExclusive / FbxDisplayLayer
    // NOT SUPPORTED
    
    // Pose
    count = 0;
    if (count) {
        n = FBX::Node("ObjectType", Property("Pose"));
        n.AddChild("Count", count);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // Deformer
    count = 0;
    if (count) {
        n = FBX::Node("ObjectType", Property("Deformer"));
        n.AddChild("Count", count);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // (template)
    count = 0;
    if (count) {
        n = FBX::Node("ObjectType", Property(""));
        n.AddChild("Count", count);
        pt = FBX::Node("PropertyTemplate", Property(""));
        p = FBX::Node("Properties70");
        pt.AddChild(p);
        n.AddChild(pt);
        object_nodes.push_back(n);
        total_count += count;
    }
    
    // now write it all
    FBX::Node defs("Definitions");
    defs.AddChild("Version", int32_t(100));
    defs.AddChild("Count", int32_t(total_count));
    for (auto &n : object_nodes) { defs.AddChild(n); }
    defs.Dump(outfile);
}

aiNode* get_node_for_mesh(unsigned int meshIndex, aiNode* node)
{
    for (size_t i = 0; i < node->mNumMeshes; ++i) {
        if (node->mMeshes[i] == meshIndex) {
            return node;
        }
    }
    for (size_t i = 0; i < node->mNumChildren; ++i) {
        aiNode* ret = get_node_for_mesh(meshIndex, node->mChildren[i]);
        if (ret) { return ret; }
    }
    return nullptr;
}

aiMatrix4x4 get_world_transform(const aiNode* node, const aiScene* scene)
{
    std::vector<const aiNode*> node_chain;
    while (node != scene->mRootNode) {
        node_chain.push_back(node);
        node = node->mParent;
    }
    aiMatrix4x4 transform;
    for (auto n = node_chain.rbegin(); n != node_chain.rend(); ++n) {
        transform *= (*n)->mTransformation;
    }
    return transform;
}


void FBXExporter::WriteObjects ()
{
    // numbers should match those given in definitions! make sure to check
    StreamWriterLE outstream(outfile);
    FBX::Node object_node("Objects");
    object_node.Begin(outstream);
    object_node.EndProperties(outstream);
    
    // geometry (aiMesh)
    std::vector<int64_t> mesh_uids;
    for (size_t mi = 0; mi < mScene->mNumMeshes; ++mi) {
        // it's all about this mesh
        aiMesh* m = mScene->mMeshes[mi];
        
        // start the node record
        FBX::Node n("Geometry");
        int64_t uid = generate_uid();
        mesh_uids.push_back(uid);
        n.AddProperty(uid);
        n.AddProperty(FBX::SEPARATOR + "Geometry");
        n.AddProperty("Mesh");
        n.Begin(outstream);
        n.DumpProperties(outstream);
        n.EndProperties(outstream);
        
        // output vertex data - each vertex should be unique (probably)
        std::vector<double> flattened_vertices;
        // index of original vertex in vertex data vector
        std::vector<int32_t> vertex_indices;
        // map of vertex value to its index in the data vector
        std::map<aiVector3D,size_t> index_by_vertex_value;
        size_t index = 0;
        for (size_t vi = 0; vi < m->mNumVertices; ++vi) {
            aiVector3D vtx = m->mVertices[vi];
            auto elem = index_by_vertex_value.find(vtx);
            if (elem == index_by_vertex_value.end()) {
                vertex_indices.push_back(index);
                index_by_vertex_value[vtx] = index;
                flattened_vertices.push_back(vtx[0]);
                flattened_vertices.push_back(vtx[1]);
                flattened_vertices.push_back(vtx[2]);
                ++index;
            } else {
                vertex_indices.push_back(elem->second);
            }
        }
        WritePropertyNode("Vertices", flattened_vertices, outstream);
        
        // output polygon data as a flattened array of vertex indices.
        // the last vertex index of each polygon is negated and - 1
        std::vector<int32_t> polygon_data;
        for (size_t fi = 0; fi < m->mNumFaces; ++fi) {
            const aiFace &f = m->mFaces[fi];
            for (size_t pvi = 0; pvi < f.mNumIndices - 1; ++pvi) {
                polygon_data.push_back(vertex_indices[f.mIndices[pvi]]);
            }
            polygon_data.push_back(-1 - vertex_indices[f.mIndices[f.mNumIndices-1]]);
        }
        WritePropertyNode("PolygonVertexIndex", polygon_data, outstream);
        
        // here could be edges but they're insane.
        // it's optional anyway, so let's ignore it.
        
        WritePropertyNode("GeometryVersion", int32_t(124), outstream);
        
        // normals, if any
        if (m->HasNormals()) {
            FBX::Node normals("LayerElementNormal", Property(int32_t(0)));
            normals.Begin(outstream);
            normals.DumpProperties(outstream);
            normals.EndProperties(outstream);
            WritePropertyNode("Version", int32_t(101), outstream);
            WritePropertyNode("Name", "", outstream);
            WritePropertyNode("MappingInformationType", "ByPolygonVertex", outstream);
            // TODO: vertex-normals or indexed normals when appropriate
            WritePropertyNode("ReferenceInformationType", "Direct", outstream);
            std::vector<double> normal_data;
            normal_data.reserve(3 * polygon_data.size());
            for (size_t fi = 0; fi < m->mNumFaces; ++fi) {
                const aiFace &f = m->mFaces[fi];
                for (size_t pvi = 0; pvi < f.mNumIndices; ++pvi) {
                    const aiVector3D &n = m->mNormals[f.mIndices[pvi]];
                    normal_data.push_back(n.x);
                    normal_data.push_back(n.y);
                    normal_data.push_back(n.z);
                }
            }
            WritePropertyNode("Normals", normal_data, outstream);
            // note: version 102 has a NormalsW also... not sure what it is,
            // so we can stick with version 101 for now.
            normals.End(outstream, true);
        }
        
        // uvs, if any
        for (size_t uvi = 0; uvi < m->GetNumUVChannels(); ++uvi) {
            if (m->mNumUVComponents[uvi] > 2) {
                // FBX only supports 2-channel UV maps...
                // or at least i'm not sure how to indicate a different number
                std::stringstream err;
                err << "Only 2-channel UV maps supported by FBX,";
                err << " but mesh " << mi;
                if (m->mName.length) { err << " (" << m->mName.C_Str() << ")"; }
                err << " UV map " << uvi << " has " << m->mNumUVComponents[uvi];
                err << " components! Data will be preserved,";
                err << " but may be incorrectly interpreted on load.";
                DefaultLogger::get()->warn(err.str());
            }
            FBX::Node uv("LayerElementUV", Property(int32_t(uvi)));
            uv.Begin(outstream);
            uv.DumpProperties(outstream);
            uv.EndProperties(outstream);
            WritePropertyNode("Version", int32_t(101), outstream);
            // it doesn't seem like assimp keeps the uv map name,
            // so just leave it blank.
            WritePropertyNode("Name", "", outstream);
            WritePropertyNode("MappingInformationType", "ByPolygonVertex", outstream);
            WritePropertyNode("ReferenceInformationType", "IndexToDirect", outstream);
            
            std::vector<double> uv_data;
            std::vector<int32_t> uv_indices;
            std::map<aiVector3D,int32_t> index_by_uv;
            size_t index = 0;
            for (size_t fi = 0; fi < m->mNumFaces; ++fi) {
                const aiFace &f = m->mFaces[fi];
                for (size_t pvi = 0; pvi < f.mNumIndices; ++pvi) {
                    const aiVector3D &uv = m->mTextureCoords[uvi][f.mIndices[pvi]];
                    auto elem = index_by_uv.find(uv);
                    if (elem == index_by_uv.end()) {
                        index_by_uv[uv] = index;
                        uv_indices.push_back(index);
                        for (size_t x = 0; x < m->mNumUVComponents[uvi]; ++x) {
                            uv_data.push_back(uv[x]);
                        }
                        ++index;
                    } else {
                        uv_indices.push_back(elem->second);
                    }
                }
            }
            WritePropertyNode("UV", uv_data, outstream);
            WritePropertyNode("UVIndex", uv_indices, outstream);
            uv.End(outstream, true);
        }
        
        // i'm not really sure why this material section exists,
        // as the material is linked via "Connections".
        // it seems to always have the same "0" value.
        FBX::Node mat("LayerElementMaterial", Property(int32_t(0)));
        mat.AddChild("Version", int32_t(101));
        mat.AddChild("Name", "");
        mat.AddChild("MappingInformationType", "AllSame");
        mat.AddChild("ReferenceInformationType", "IndexToDirect");
        std::vector<int32_t> mat_indices = {0};
        mat.AddChild("Materials", mat_indices);
        mat.Dump(outstream);
        
        // finally we have the layer specifications,
        // which select the normals / UV set / etc to use.
        // TODO: handle multiple uv sets correctly?
        FBX::Node layer("Layer", Property(int32_t(0)));
        layer.AddChild("Version", int32_t(100));
        FBX::Node le("LayerElement");
        le.AddChild("Type", "LayerElementNormal");
        le.AddChild("TypedIndex", int32_t(0));
        layer.AddChild(le);
        le = FBX::Node("LayerElement");
        le.AddChild("Type", "LayerElementMaterial");
        le.AddChild("TypedIndex", int32_t(0));
        layer.AddChild(le);
        le = FBX::Node("LayerElement");
        le.AddChild("Type", "LayerElementUV");
        le.AddChild("TypedIndex", int32_t(0));
        layer.AddChild(le);
        layer.Dump(outstream);
        
        // finish the node record
        n.End(outstream, true);
    }
    
    // aiMaterial
    std::vector<int64_t> material_uids;
    for (size_t i = 0; i < mScene->mNumMaterials; ++i) {
        // it's all about this material
        aiMaterial* m = mScene->mMaterials[i];
        
        // these are used to recieve material data
        float f; aiColor3D c;
        
        // start the node record
        FBX::Node n("Material");
        
        int64_t uid = generate_uid();
        material_uids.push_back(uid);
        n.AddProperty(uid);
        
        aiString name;
        m->Get(AI_MATKEY_NAME, name);
        n.AddProperty(name.C_Str() + FBX::SEPARATOR + "Material");
        
        n.AddProperty("");
        
        n.AddChild("Version", int32_t(102));
        f = 0;
        m->Get(AI_MATKEY_SHININESS, f);
        bool phong = (f > 0);
        if (phong) {
            n.AddChild("ShadingModel", "phong");
        } else {
            n.AddChild("ShadingModel", "lambert");
        }
        n.AddChild("MultiLayer", int32_t(0));
        
        FBX::Node p("Properties70");
        
        // materials exported using the FBX SDK have two sets of fields.
        // there are the properties specified in the PropertyTemplate,
        // which are those supported by the modernFBX SDK,
        // and an extra set of properties with simpler names.
        // The extra properties are a legacy material system from pre-2009.
        //
        // In the modern system, each property has "color" and "factor".
        // Generally the interpretation of these seems to be
        // that the colour is multiplied by the factor before use,
        // but this is not always clear-cut.
        //
        // Usually assimp only stores the colour,
        // so we can just leave the factors at the default "1.0".
        
        // first we can export the "standard" properties
        if (m->Get(AI_MATKEY_COLOR_AMBIENT, c) == aiReturn_SUCCESS) {
            p.AddP70colorA("AmbientColor", c.r, c.g, c.b);
            //p.AddP70numberA("AmbientFactor", 1.0);
        }
        if (m->Get(AI_MATKEY_COLOR_DIFFUSE, c) == aiReturn_SUCCESS) {
            p.AddP70colorA("DiffuseColor", c.r, c.g, c.b);
            //p.AddP70numberA("DiffuseFactor", 1.0);
        }
        if (m->Get(AI_MATKEY_COLOR_TRANSPARENT, c) == aiReturn_SUCCESS) {
            // "TransparentColor" / "TransparencyFactor"...
            // thanks FBX, for your insightful interpretation of consistency
            p.AddP70colorA("TransparentColor", c.r, c.g, c.b);
            // TransparencyFactor defaults to 0.0, so set it to 1.0.
            // note: Maya always sets this to 1.0,
            // so we can't use it sensibly as "Opacity".
            // In stead we rely on the legacy "Opacity" value, below.
            // Blender also relies on "Opacity" not "TransparencyFactor",
            // probably for a similar reason.
            p.AddP70numberA("TransparencyFactor", 1.0);
        }
        if (m->Get(AI_MATKEY_COLOR_REFLECTIVE, c) == aiReturn_SUCCESS) {
            p.AddP70colorA("ReflectionColor", c.r, c.g, c.b);
        }
        if (m->Get(AI_MATKEY_REFLECTIVITY, f) == aiReturn_SUCCESS) {
            p.AddP70numberA("ReflectionFactor", f);
        }
        if (phong) {
            if (m->Get(AI_MATKEY_COLOR_SPECULAR, c) == aiReturn_SUCCESS) {
                p.AddP70colorA("SpecularColor", c.r, c.g, c.b);
            }
            if (m->Get(AI_MATKEY_SHININESS_STRENGTH, f) == aiReturn_SUCCESS) {
                p.AddP70numberA("ShininessFactor", f);
            }
            if (m->Get(AI_MATKEY_SHININESS, f) == aiReturn_SUCCESS) {
                p.AddP70numberA("ShininessExponent", f);
            }
            if (m->Get(AI_MATKEY_REFLECTIVITY, f) == aiReturn_SUCCESS) {
                p.AddP70numberA("ReflectionFactor", f);
            }
        }
        
        // Now the legacy system.
        // For safety let's include it.
        // thrse values don't exist in the property template,
        // and usualy are completely ignored when loading.
        // One notable exception is the "Opacity" property,
        // which Blender uses as (1.0 - alpha).
        c.r = 0; c.g = 0; c.b = 0;
        m->Get(AI_MATKEY_COLOR_EMISSIVE, c);
        p.AddP70vector("Emissive", c.r, c.g, c.b);
        c.r = 0.2; c.g = 0.2; c.b = 0.2;
        m->Get(AI_MATKEY_COLOR_AMBIENT, c);
        p.AddP70vector("Ambient", c.r, c.g, c.b);
        c.r = 0.8; c.g = 0.8; c.b = 0.8;
        m->Get(AI_MATKEY_COLOR_DIFFUSE, c);
        p.AddP70vector("Diffuse", c.r, c.g, c.b);
        // The FBX SDK determines "Opacity" from transparency colour (RGB)
        // and factor (F) as: O = (1.0 - F * ((R + G + B) / 3)).
        // However we actually have an opacity value,
        // so we should take it from AI_MATKEY_OPACITY if possible.
        // It might make more sense to use TransparencyFactor,
        // but Blender actually loads "Opacity" correctly, so let's use it.
        f = 1.0;
        if (m->Get(AI_MATKEY_COLOR_TRANSPARENT, c) == aiReturn_SUCCESS) {
            f = 1.0 - ((c.r + c.g + c.b) / 3);
        }
        m->Get(AI_MATKEY_OPACITY, f);
        p.AddP70double("Opacity", f);
        if (phong) {
            // specular color is multiplied by shininess_strength
            c.r = 0.2; c.g = 0.2; c.b = 0.2;
            m->Get(AI_MATKEY_COLOR_SPECULAR, c);
            f = 1.0;
            m->Get(AI_MATKEY_SHININESS_STRENGTH, f);
            p.AddP70vector("Specular", f*c.r, f*c.g, f*c.b);
            f = 20.0;
            m->Get(AI_MATKEY_SHININESS, f);
            p.AddP70double("Shininess", f);
            // Legacy "Reflectivity" is F*F*((R+G+B)/3),
            // where F is the proportion of light reflected (AKA reflectivity),
            // and RGB is the reflective colour of the material.
            // No idea why, but we might as well set it the same way.
            f = 0.0;
            m->Get(AI_MATKEY_REFLECTIVITY, f);
            c.r = 1.0, c.g = 1.0, c.b = 1.0;
            m->Get(AI_MATKEY_COLOR_REFLECTIVE, c);
            p.AddP70double("Reflectivity", f*f*((c.r+c.g+c.b)/3.0));
        }
        
        n.AddChild(p);
        
        n.Dump(outstream);
    }
    
    // we need to look up all the images we're using,
    // so we can generate uids, and eliminate duplicates.
    std::map<std::string, int64_t> uid_by_image;
    for (size_t i = 0; i < mScene->mNumMaterials; ++i) {
        aiString texpath;
        aiMaterial* mat = mScene->mMaterials[i];
        for (
            size_t tt = aiTextureType_DIFFUSE;
            tt < aiTextureType_UNKNOWN;
            ++tt
        ){
            const aiTextureType textype = static_cast<aiTextureType>(tt);
            const size_t texcount = mat->GetTextureCount(textype);
            for (size_t j = 0; j < texcount; ++j) {
                mat->GetTexture(textype, j, &texpath);
                const std::string texstring = texpath.C_Str();
                auto elem = uid_by_image.find(texstring);
                if (elem == uid_by_image.end()) {
                    uid_by_image[texstring] = generate_uid();
                }
            }
        }
    }
    
    // FbxVideo - stores images used by textures.
    for (const auto &it : uid_by_image) {
        if (it.first.compare(0, 1, "*") == 0) {
            // TODO: embedded textures
            continue;
        }
        FBX::Node n("Video");
        const int64_t& uid = it.second;
        const std::string name = ""; // TODO: ... name???
        n.AddProperties(uid, name + FBX::SEPARATOR + "Video", "Clip");
        n.AddChild("Type", "Clip");
        FBX::Node p("Properties70");
        // TODO: get full path... relative path... etc... ugh...
        // for now just use the same path for everything,
        // and hopefully one of them will work out.
        const std::string& path = it.first;
        p.AddP70("Path", "KString", "XRefUrl", "", path);
        n.AddChild(p);
        n.AddChild("UseMipMap", int32_t(0));
        n.AddChild("Filename", path);
        n.AddChild("RelativeFilename", path);
        n.Dump(outstream);
    }
    
    // Textures
    // referenced by material_index/texture_type pairs.
    std::map<std::pair<size_t,size_t>,int64_t> texture_uids;
    const std::map<aiTextureType,std::string> prop_name_by_tt = {
        {aiTextureType_DIFFUSE, "DiffuseColor"},
        {aiTextureType_SPECULAR, "SpecularColor"},
        {aiTextureType_AMBIENT, "AmbientColor"},
        {aiTextureType_EMISSIVE, "EmissiveColor"},
        {aiTextureType_HEIGHT, "Bump"},
        {aiTextureType_NORMALS, "NormalMap"},
        {aiTextureType_SHININESS, "ShininessExponent"},
        {aiTextureType_OPACITY, "TransparentColor"},
        {aiTextureType_DISPLACEMENT, "DisplacementColor"},
        //{aiTextureType_LIGHTMAP, "???"},
        {aiTextureType_REFLECTION, "ReflectionColor"}
        //{aiTextureType_UNKNOWN, ""}
    };
    for (size_t i = 0; i < mScene->mNumMaterials; ++i) {
        // textures are attached to materials
        aiMaterial* mat = mScene->mMaterials[i];
        int64_t material_uid = material_uids[i];
        
        for (
            size_t j = aiTextureType_DIFFUSE;
            j < aiTextureType_UNKNOWN;
            ++j
        ) {
            const aiTextureType tt = static_cast<aiTextureType>(j);
            size_t n = mat->GetTextureCount(tt);
            
            if (n < 1) { // no texture of this type
                continue;
            }
            
            if (n > 1) {
                // TODO: multilayer textures
                std::stringstream err;
                err << "Multilayer textures not supported (for now),";
                err << " skipping texture type " << j;
                err << " of material " << i;
                DefaultLogger::get()->warn(err.str());
            }
            
            // get image path for this (single-image) texture
            aiString tpath;
            if (mat->GetTexture(tt, 0, &tpath) != aiReturn_SUCCESS) {
                std::stringstream err;
                err << "Failed to get texture 0 for texture of type " << tt;
                err << " on material " << i;
                err << ", however GetTextureCount returned 1.";
                throw DeadlyExportError(err.str());
            }
            const std::string texture_path(tpath.C_Str());
            
            // get connected image uid
            auto elem = uid_by_image.find(texture_path);
            if (elem == uid_by_image.end()) {
                // this should never happen
                std::stringstream err;
                err << "Failed to find video element for texture with path";
                err << " \"" << texture_path << "\"";
                err << ", type " << j << ", material " << i;
                throw DeadlyExportError(err.str());
            }
            const int64_t image_uid = elem->second;
            
            // get the name of the material property to connect to
            auto elem2 = prop_name_by_tt.find(tt);
            if (elem2 == prop_name_by_tt.end()) {
                // don't know how to handle this type of texture,
                // so skip it.
                std::stringstream err;
                err << "Not sure how to handle texture of type " << j;
                err << " on material " << i;
                err << ", skipping...";
                DefaultLogger::get()->warn(err.str());
                continue;
            }
            const std::string& prop_name = elem2->second;
            
            // generate a uid for this texture
            const int64_t texture_uid = generate_uid();
            
            // link the texture to the material
            FBX::Node c("C");
            c.AddProperties("OP", texture_uid, material_uid, prop_name);
            connections.push_back(c);
            
            // link the image data to the texture
            c = FBX::Node("C");
            c.AddProperties("OO", image_uid, texture_uid);
            connections.push_back(c);
            
            // now write the actual texture node
            FBX::Node tnode("Texture");
            // TODO: some way to determine texture name?
            const std::string texture_name = "" + FBX::SEPARATOR + "Texture";
            tnode.AddProperties(texture_uid, texture_name, "");
            // there really doesn't seem to be a better type than this:
            tnode.AddChild("Type", "TextureVideoClip");
            tnode.AddChild("Version", int32_t(202));
            tnode.AddChild("TextureName", texture_name);
            FBX::Node p("Properties70");
            p.AddP70enum("CurrentTextureBlendMode", 0); // TODO: verify
            //p.AddP70string("UVSet", ""); // TODO: how should this work?
            p.AddP70bool("UseMaterial", 1);
            tnode.AddChild(p);
            // can't easily detrmine which texture path will be correct,
            // so just store what we have in every field.
            // these being incorrect is a common problem with FBX anyway.
            tnode.AddChild("FileName", texture_path);
            tnode.AddChild("RelativeFilename", texture_path);
            tnode.AddChild("ModelUVTranslation", double(0.0), double(0.0));
            tnode.AddChild("ModelUVScaling", double(1.0), double(1.0));
            tnode.AddChild("Texture_Alpha_Soutce", "None");
            tnode.AddChild("Cropping", int32_t(0), int32_t(0), int32_t(0), int32_t(0));
            tnode.Dump(outstream);
        }
    }
    
    // bones.
    //
    // output structure:
    // subset of node heirarchy that are "skeleton",
    // i.e. do not have meshes but only bones.
    // but.. i'm not sure how anyone could guarantee that...
    //
    // input...
    // well, for each mesh it has "bones",
    // and the bone names correspond to nodes.
    // of course we also need the parent nodes,
    // as they give some of the transform........
    //
    // well. we can assume a sane input, i suppose.
    // 
    // so input is the bone node heirarchy,
    // with an extra thing for the transformation of the BONE in MESH space.
    //
    // output is a set of bone nodes,
    // a "bindpose" which indicates the default local transform of all bones,
    // and a set of "deformers".
    // each deformer is parented to a mesh geometry,
    // and has one or more "subdeformer"s as children.
    // each subdeformer has one bone node as a child,
    // and represents the influence of that bone on the grandparent mesh.
    // the subdeformer has a list of indices, and weights,
    // with indices specifying vertex indices,
    // and weights specifying the correspoding influence of this bone.
    // it also has Transform and TransformLink elements,
    // specifying the transform of the MESH in BONE space,
    // and the transformation of the BONE in WORLD space,
    // likely in the bindpose.
    //
    // the input bone structure is different but similar,
    // storing the number of weights for this bone,
    // and an array of (vertex index, weight) pairs.
    //
    // one sticky point is that the number of vertices may not match,
    // because assimp splits vertices by normal, uv, etc.
    
    // first we should mark all the skeleton nodes,
    // so that they can be treated as LimbNode in stead of Mesh or Null.
    // at the same time we can build up a map of bone nodes.
    std::unordered_set<const aiNode*> limbnodes;
    std::map<std::string,aiNode*> node_by_bone;
    for (size_t mi = 0; mi < mScene->mNumMeshes; ++mi) {
        const aiMesh* m = mScene->mMeshes[mi];
        for (size_t bi =0; bi < m->mNumBones; ++bi) {
            const aiBone* b = m->mBones[bi];
            const std::string name(b->mName.C_Str());
            if (node_by_bone.count(name) > 0) {
                // already processed, skip
                continue;
            }
            aiNode* n = mScene->mRootNode->FindNode(b->mName);
            if (!n) {
                // this should never happen
                std::stringstream err;
                err << "Failed to find node for bone: \"" << name << "\"";
                throw DeadlyExportError(err.str());
            }
            node_by_bone[name] = n;
            limbnodes.insert(n);
            if (n == mScene->mRootNode) { continue; }
            // mark all parent nodes as skeleton as well,
            // up until we find the root node,
            // or else the node containing the mesh,
            // or else the parent of a node containig the mesh.
            for (
                const aiNode* parent = n->mParent;
                parent != mScene->mRootNode;
                parent = parent->mParent
            ) {
                bool end = false;
                for (size_t i = 0; i < parent->mNumMeshes; ++i) {
                    if (parent->mMeshes[i] == mi) {
                        end = true;
                        break;
                    }
                }
                for (size_t j = 0; j < parent->mNumChildren; ++j) {
                    aiNode* child = parent->mChildren[j];
                    for (size_t i = 0; i < child->mNumMeshes; ++i) {
                        if (child->mMeshes[i] == mi) {
                            end = true;
                            break;
                        }
                    }
                    if (end) { break; }
                }
                if (end) { break; }
                limbnodes.insert(parent);
            }
        }
    }
    
    // now, for each aiMesh, we need to export a deformer,
    // and for each aiBone a subdeformer,
    // which should have all the skinning info.
    // these will need to be connected properly to the mesh,
    // and we can do that all now.
    for (size_t mi = 0; mi < mScene->mNumMeshes; ++mi) {
        const aiMesh* m = mScene->mMeshes[mi];
        if (!m->HasBones()) {
            continue;
        }
        // make a deformer for this mesh
        int64_t deformer_uid = generate_uid();
        FBX::Node dnode("Deformer");
        dnode.AddProperties(deformer_uid, FBX::SEPARATOR + "Deformer", "Skin");
        dnode.AddChild("Version", int32_t(101));
        // "acuracy"... this is not a typo....
        dnode.AddChild("Link_DeformAcuracy", double(50));
        dnode.AddChild("SkinningType", "Linear"); // TODO: other modes?
        dnode.Dump(outstream);
        
        // connect it
        FBX::Node c("C");
        c.AddProperties("OO", deformer_uid, mesh_uids[mi]);
        connections.push_back(c); // TODO: emplace_back
        
        // we will be indexing by vertex...
        // but there might be a different number of "vertices"
        // between assimp and our output FBX.
        // this code is cut-and-pasted from the geometry section above...
        // ideally this should not be so.
        // ---
        // index of original vertex in vertex data vector
        std::vector<int32_t> vertex_indices;
        // map of vertex value to its index in the data vector
        std::map<aiVector3D,size_t> index_by_vertex_value;
        size_t index = 0;
        for (size_t vi = 0; vi < m->mNumVertices; ++vi) {
            aiVector3D vtx = m->mVertices[vi];
            auto elem = index_by_vertex_value.find(vtx);
            if (elem == index_by_vertex_value.end()) {
                vertex_indices.push_back(index);
                index_by_vertex_value[vtx] = index;
                ++index;
            } else {
                vertex_indices.push_back(elem->second);
            }
        }
        
        // first get this mesh's position in world space,
        // as we'll need it for each subdeformer.
        //
        // ...of course taking the position of the MESH doesn't make sense,
        // as it can be instanced to many nodes.
        // All we can do is assume no instancing,
        // and take the first node we find that contains the mesh.
        //
        // We could in stead take the transform from the bone's node,
        // but there's no guarantee that the bone is in the bindpose,
        // so this would be even less reliable.
        aiNode* mesh_node = get_node_for_mesh(mi, mScene->mRootNode);
        aiMatrix4x4 mesh_node_xform = get_world_transform(mesh_node, mScene);
        
        // now make a subdeformer for each bone
        for (size_t bi =0; bi < m->mNumBones; ++bi) {
            const aiBone* b = m->mBones[bi];
            const std::string name(b->mName.C_Str());
            const int64_t subdeformer_uid = generate_uid();
            FBX::Node sdnode("Deformer");
            sdnode.AddProperties(
                subdeformer_uid, FBX::SEPARATOR + "SubDeformer", "Cluster"
            );
            sdnode.AddChild("Version", int32_t(100));
            sdnode.AddChild("UserData", "", "");
            
            // get indices and weights
            std::vector<int32_t> subdef_indices;
            std::vector<double> subdef_weights;
            int32_t last_index = -1;
            for (size_t wi = 0; wi < b->mNumWeights; ++wi) {
                int32_t vi = vertex_indices[b->mWeights[wi].mVertexId];
                if (vi == last_index) {
                    // only for vertices we exported to fbx
                    // TODO, FIXME: this assumes identically-located vertices
                    // will always deform in the same way.
                    // as assimp doesn't store a separate list of "positions",
                    // there's not much that can be done about this
                    // other than assuming that identical position means
                    // identical vertex.
                    continue;
                }
                subdef_indices.push_back(vi);
                subdef_weights.push_back(b->mWeights[wi].mWeight);
                last_index = vi;
            }
            // yes, "indexes"
            sdnode.AddChild("Indexes", subdef_indices);
            sdnode.AddChild("Weights", subdef_weights);
            // transform is the transform of the mesh, but in bone space...
            // should be the inverse of assimp's mOffsetMatrix.
            aiMatrix4x4 tr = b->mOffsetMatrix;
            /// ... however mOffsetMatrix is clearly the mesh in bone space.
            // this directly contradicts the docs :/
            //tr.Inverse();
            sdnode.AddChild("Transform", tr);
            // transformlink should be the position of the bone in world space,
            // in the bind pose.
            // as mOffsetMatrix is in fact for the mesh in bone space,
            // to get the bone in world space we need to take the inverse,
            // and multiply by the mesh's world space position
            tr = b->mOffsetMatrix;
            tr.Inverse();
            // (not that it makes sense to take the mesh position,
            // as it could be instanced, but there's no better alternative)
            tr *= mesh_node_xform;
            sdnode.AddChild("TransformLink", tr);
            
            // done
            sdnode.Dump(outstream);
            
            // lastly, connect to the parent deformer
            c = FBX::Node("C");
            c.AddProperties("OO", subdeformer_uid, deformer_uid);
            connections.push_back(c); // TODO: emplace_back
        }
        
        
    }
    
    
    // TODO: cameras, lights
    
    // write nodes (i.e. model heirarchy)
    // start at root node
    WriteModelNodes(
        outstream, mScene->mRootNode, 0, mesh_uids, material_uids, limbnodes
    );
    
    object_node.End(outstream, true);
}

// convenience map of magic node name strings to FBX properties,
// including the expected type of transform.
const std::map<std::string,std::pair<std::string,char>> transform_types = {
    {"Translation", {"Lcl Translation", 't'}},
    {"RotationOffset", {"RotationOffset", 't'}},
    {"RotationPivot", {"RotationPivot", 't'}},
    {"PreRotation", {"PreRotation", 'r'}},
    {"Rotation", {"Lcl Rotation", 'r'}},
    {"PostRotation", {"PostRotation", 'r'}},
    {"RotationPivotInverse", {"RotationPivotInverse", 'i'}},
    {"ScalingOffset", {"ScalingOffset", 't'}},
    {"ScalingPivot", {"ScalingPivot", 't'}},
    {"Scaling", {"Lcl Scaling", 's'}},
    {"ScalingPivotInverse", {"ScalingPivotInverse", 'i'}},
    {"GeometricScaling", {"GeometricScaling", 's'}},
    {"GeometricRotation", {"GeometricRotation", 'r'}},
    {"GeometricTranslation", {"GeometricTranslation", 't'}}
};

// write a single model node to the stream
void WriteModelNode(
    StreamWriterLE& outstream,
    const aiNode* node,
    int64_t node_uid,
    const std::string& type,
    const std::vector<std::pair<std::string,aiVector3D>>& transform_chain,
    TransformInheritance inherit_type=TransformInheritance_RSrs
){
    const aiVector3D zero = {0, 0, 0};
    const aiVector3D one = {1, 1, 1};
    FBX::Node m("Model");
    std::string name = node->mName.C_Str() + FBX::SEPARATOR + "Model";
    m.AddProperties(node_uid, name, type);
    m.AddChild("Version", int32_t(232));
    FBX::Node p("Properties70");
    p.AddP70bool("RotationActive", 1);
    p.AddP70int("DefaultAttributeIndex", 0);
    p.AddP70enum("InheritType", inherit_type);
    if (transform_chain.empty()) {
        // decompose 4x4 transform matrix into TRS
        aiVector3D t, r, s;
        node->mTransformation.Decompose(s, r, t);
        if (t != zero) {
            p.AddP70(
                "Lcl Translation", "Lcl Translation", "", "A",
                double(t.x), double(t.y), double(t.z)
            );
        }
        if (r != zero) {
            p.AddP70(
                "Lcl Rotation", "Lcl Rotation", "", "A",
                double(DEG*r.x), double(DEG*r.y), double(DEG*r.z)
            );
        }
        if (s != one) {
            p.AddP70(
                "Lcl Scaling", "Lcl Scaling", "", "A",
                double(s.x), double(s.y), double(s.z)
            );
        }
    } else {
        // apply the transformation chain
        for (auto &item : transform_chain) {
            auto elem = transform_types.find(item.first);
            if (elem == transform_types.end()) {
                // then this is a bug
                std::stringstream err;
                err << "unrecognized FBX transformation type: ";
                err << item.first;
                throw DeadlyExportError(err.str());
            }
            const std::string &name = elem->second.first;
            const aiVector3D &v = item.second;
            if (name.compare(0, 4, "Lcl ") == 0) {
                // special handling for animatable properties
                p.AddP70(
                    name, name, "", "A",
                    double(v.x), double(v.y), double(v.z)
                );
            } else {
                p.AddP70vector(name, v.x, v.y, v.z);
            }
        }
    }
    m.AddChild(p);
    
    // not sure what these are for,
    // but they seem to be omnipresent
    m.AddChild("Shading", Property(true));
    m.AddChild("Culling", Property("CullingOff"));
    
    m.Dump(outstream);
}

// wrapper for WriteModelNodes to create and pass a blank transform chain
void FBXExporter::WriteModelNodes(
    StreamWriterLE& s,
    const aiNode* node,
    int64_t parent_uid,
    const std::vector<int64_t>& mesh_uids,
    const std::vector<int64_t>& material_uids,
    const std::unordered_set<const aiNode*>& limbnodes
) {
    std::vector<std::pair<std::string,aiVector3D>> chain;
    WriteModelNodes(s, node, parent_uid, mesh_uids, material_uids, limbnodes, chain);
}

void FBXExporter::WriteModelNodes(
    StreamWriterLE& outstream,
    const aiNode* node,
    int64_t parent_uid,
    const std::vector<int64_t>& mesh_uids,
    const std::vector<int64_t>& material_uids,
    const std::unordered_set<const aiNode*>& limbnodes,
    std::vector<std::pair<std::string,aiVector3D>>& transform_chain
) {
    // first collapse any expanded transformation chains created by FBX import.
    std::string node_name(node->mName.C_Str());
    if (node_name.find(MAGIC_NODE_TAG) != std::string::npos) {
        if (node->mNumChildren != 1) {
            // this should never happen
            std::stringstream err;
            err << "FBX transformation node should have 1 child,";
            err << " but " << node->mNumChildren << " found";
            err << " on node \"" << node_name << "\"!";
            throw DeadlyExportError(err.str());
        }
        aiNode* next_node = node->mChildren[0];
        auto pos = node_name.find(MAGIC_NODE_TAG) + MAGIC_NODE_TAG.size() + 1;
        std::string type_name = node_name.substr(pos);
        auto elem = transform_types.find(type_name);
        if (elem == transform_types.end()) {
            // then this is a bug and should be fixed
            std::stringstream err;
            err << "unrecognized FBX transformation node";
            err << " of type " << type_name << " in node " << node_name;
            throw DeadlyExportError(err.str());
        }
        aiVector3D t, r, s;
        node->mTransformation.Decompose(s, r, t);
        switch (elem->second.second) {
        case 'i': // inverse
            // we don't need to worry about the inverse matrices
            break;
        case 't': // translation
            transform_chain.emplace_back(elem->first, t);
            break;
        case 'r': // rotation
            r *= DEG;
            transform_chain.emplace_back(elem->first, r);
            break;
        case 's': // scale
            transform_chain.emplace_back(elem->first, s);
            break;
        default:
            // this should never happen
            std::stringstream err;
            err << "unrecognized FBX transformation type code: ";
            err << elem->second.second;
            throw DeadlyExportError(err.str());
        }
        // now just continue to the next node
        WriteModelNodes(
            outstream, next_node, parent_uid, mesh_uids, material_uids,
            limbnodes, transform_chain
        );
        return;
    }
    
    int64_t node_uid = 0;
    // generate uid and connect to parent, if not the root node
    if (node != mScene->mRootNode) {
        node_uid = generate_uid();
        FBX::Node c("C");
        c.AddProperties("OO", node_uid, parent_uid);
        connections.push_back(c);
    }
    
    // what type of node is this?
    if (node == mScene->mRootNode) {
        // handled later
    } else if (node->mNumMeshes == 1) {
        // connect to child mesh, which should have been written previously
        FBX::Node c("C");
        c.AddProperties("OO", mesh_uids[node->mMeshes[0]], node_uid);
        connections.push_back(c);
        // also connect to the material for the child mesh
        c = FBX::Node("C");
        c.AddProperties(
            "OO",
            material_uids[mScene->mMeshes[node->mMeshes[0]]->mMaterialIndex],
            node_uid
        );
        connections.push_back(c);
        // write model node
        WriteModelNode(outstream, node, node_uid, "Mesh", transform_chain);
    } else if (limbnodes.count(node)) {
        WriteModelNode(outstream, node, node_uid, "LimbNode", transform_chain);
    } else {
        // generate a null node so we can add children to it
        WriteModelNode(outstream, node, node_uid, "Null", transform_chain);
    }
    
    // if more than one child mesh, make nodes for each mesh
    if (node->mNumMeshes > 1 || node == mScene->mRootNode) {
        for (size_t i = 0; i < node->mNumMeshes; ++i) {
            // make a new model node
            int64_t new_node_uid = generate_uid();
            // connect to parent node
            FBX::Node c("C");
            c.AddProperties("OO", new_node_uid, node_uid);
            connections.push_back(c);
            // connect to child mesh, which should have been written previously
            c = FBX::Node("C");
            c.AddProperties("OO", mesh_uids[node->mMeshes[i]], new_node_uid);
            connections.push_back(c);
            // also connect to the material for the child mesh
            c = FBX::Node("C");
            c.AddProperties(
                "OO",
                material_uids[mScene->mMeshes[node->mMeshes[i]]->mMaterialIndex],
                new_node_uid
            );
            connections.push_back(c);
            // write model node
            FBX::Node m("Model");
            // take name from mesh name, if it exists
            std::string name = mScene->mMeshes[node->mMeshes[i]]->mName.C_Str();
            name += FBX::SEPARATOR + "Model";
            m.AddProperties(new_node_uid, name, "Mesh");
            m.AddChild("Version", int32_t(232));
            FBX::Node p("Properties70");
            p.AddP70enum("InheritType", 1);
            m.AddChild(p);
            m.Dump(outstream);
        }
    }
    
    // now recurse into children
    for (size_t i = 0; i < node->mNumChildren; ++i) {
        WriteModelNodes(
            outstream, node->mChildren[i], node_uid, mesh_uids, material_uids,
            limbnodes
        );
    }
}

void FBXExporter::WriteConnections ()
{
    // we should have completed the connection graph already,
    // so basically just dump it here
    FBX::Node conn("Connections");
    StreamWriterLE outstream(outfile);
    conn.Begin(outstream);
    for (auto &n : connections) {
        n.Dump(outstream);
    }
    conn.End(outstream, !connections.empty());
    connections.clear();
}

#endif // ASSIMP_BUILD_NO_FBX_EXPORTER
#endif // ASSIMP_BUILD_NO_EXPORT
