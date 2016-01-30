//
// Copyright (c) 2008-2016 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
#include "Terrain.h"

#include "TerrainPatch.h"
#include "../Core/Context.h"
#include "DrawableEvents.h"
#include "Geometry.h"
#include "../Resource/Image.h"
#include "IndexBuffer.h"
#include "../IO/Log.h"
#include "Material.h"
#include "../Scene/Node.h"
#include "Octree.h"
#include "../Core/Profiler.h"
#include "../Resource/ResourceCache.h"
#include "../Resource/ResourceEvents.h"
#include "../Scene/Scene.h"
#include "VertexBuffer.h"



namespace Urho3D
{

extern const char* GEOMETRY_CATEGORY;

static const Vector3 DEFAULT_SPACING(1.0f, 0.25f, 1.0f);
static const unsigned MAX_LOD_LEVELS = 4;
static const int DEFAULT_PATCH_SIZE = 32;
static const int MIN_PATCH_SIZE = 4;
static const int MAX_PATCH_SIZE = 128;
static const unsigned STITCH_NORTH = 1;
static const unsigned STITCH_SOUTH = 2;
static const unsigned STITCH_WEST = 4;
static const unsigned STITCH_EAST = 8;

inline void GrowUpdateRegion(IntRect& updateRegion, int x, int y)
{
    if (updateRegion.left_ < 0)
    {
        updateRegion.left_ = updateRegion.right_ = x;
        updateRegion.top_ = updateRegion.bottom_ = y;
    }
    else
    {
        if (x < updateRegion.left_)
            updateRegion.left_ = x;
        if (x > updateRegion.right_)
            updateRegion.right_ = x;
        if (y < updateRegion.top_)
            updateRegion.top_ = y;
        if (y > updateRegion.bottom_)
            updateRegion.bottom_ = y;
    }
}
Terrain::Terrain(Context* context) :
    Component(context),
    indexBuffer_(new IndexBuffer(context)),
    spacing_(DEFAULT_SPACING),
    lastSpacing_(Vector3::ZERO),
    patchWorldOrigin_(Vector2::ZERO),
    patchWorldSize_(Vector2::ZERO),
    numVertices_(IntVector2::ZERO),
    lastNumVertices_(IntVector2::ZERO),
    numPatches_(IntVector2::ZERO),
    patchSize_(DEFAULT_PATCH_SIZE),
    lastPatchSize_(0),
    numLodLevels_(1),
    maxLodLevels_(MAX_LOD_LEVELS),
    occlusionLodLevel_(M_MAX_UNSIGNED),
    smoothing_(false),
    visible_(true),
    castShadows_(false),
    occluder_(false),
    occludee_(true),
    viewMask_(DEFAULT_VIEWMASK),
    lightMask_(DEFAULT_LIGHTMASK),
    shadowMask_(DEFAULT_SHADOWMASK),
    zoneMask_(DEFAULT_ZONEMASK),
    drawDistance_(0.0f),
    shadowDistance_(0.0f),
    lodBias_(1.0f),
    maxLights_(0),
    recreateTerrain_(false)
{
    indexBuffer_->SetShadowed(true);
}

Terrain::~Terrain()
{
}

void Terrain::RegisterObject(Context* context)
{
    context->RegisterFactory<Terrain>(GEOMETRY_CATEGORY);

    URHO3D_ACCESSOR_ATTRIBUTE("Is Enabled", IsEnabled, SetEnabled, bool, true, AM_DEFAULT);
    URHO3D_MIXED_ACCESSOR_ATTRIBUTE("Height Map", GetHeightMapAttr, SetHeightMapAttr, ResourceRef, ResourceRef(Image::GetTypeStatic()), AM_DEFAULT);
    URHO3D_MIXED_ACCESSOR_ATTRIBUTE("Material", GetMaterialAttr, SetMaterialAttr, ResourceRef, ResourceRef(Material::GetTypeStatic()), AM_DEFAULT);
    URHO3D_ATTRIBUTE("Vertex Spacing", Vector3, spacing_, DEFAULT_SPACING, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Patch Size", GetPatchSize, SetPatchSizeAttr, int, DEFAULT_PATCH_SIZE, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Max LOD Levels", GetMaxLodLevels, SetMaxLodLevelsAttr, unsigned, MAX_LOD_LEVELS, AM_DEFAULT);
    URHO3D_ATTRIBUTE("Smooth Height Map", bool, smoothing_, false, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Is Occluder", IsOccluder, SetOccluder, bool,  false, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Can Be Occluded", IsOccludee, SetOccludee, bool, true, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Cast Shadows", GetCastShadows, SetCastShadows, bool, false, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Draw Distance", GetDrawDistance, SetDrawDistance, float, 0.0f, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Shadow Distance", GetShadowDistance, SetShadowDistance, float, 0.0f, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("LOD Bias", GetLodBias, SetLodBias, float, 1.0f, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Max Lights", GetMaxLights, SetMaxLights, unsigned, 0, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("View Mask", GetViewMask, SetViewMask, unsigned, DEFAULT_VIEWMASK, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Light Mask", GetLightMask, SetLightMask, unsigned, DEFAULT_LIGHTMASK, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Shadow Mask", GetShadowMask, SetShadowMask, unsigned, DEFAULT_SHADOWMASK, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Zone Mask", GetZoneMask, SetZoneMask, unsigned, DEFAULT_ZONEMASK, AM_DEFAULT);
    URHO3D_ACCESSOR_ATTRIBUTE("Occlusion LOD level", GetOcclusionLodLevel, SetOcclusionLodLevelAttr, unsigned, M_MAX_UNSIGNED, AM_DEFAULT);
}

void Terrain::OnSetAttribute(const AttributeInfo& attr, const Variant& src)
{
    Serializable::OnSetAttribute(attr, src);

    // Change of any non-accessor attribute requires recreation of the terrain
    if (!attr.accessor_)
        recreateTerrain_ = true;
}

void Terrain::ApplyAttributes()
{
    if (recreateTerrain_)
        CreateGeometry();
}

void Terrain::OnSetEnabled()
{
    bool enabled = IsEnabledEffective();

    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetEnabled(enabled);
    }
}

void Terrain::SetPatchSize(int size)
{
    if (size < MIN_PATCH_SIZE || size > MAX_PATCH_SIZE || !IsPowerOfTwo(size))
        return;

    if (size != patchSize_)
    {
        patchSize_ = size;

        CreateGeometry();
        MarkNetworkUpdate();
    }
}

void Terrain::SetSpacing(const Vector3& spacing)
{
    if (spacing != spacing_)
    {
        spacing_ = spacing;

        CreateGeometry();
        MarkNetworkUpdate();
    }
}

void Terrain::SetMaxLodLevels(unsigned levels)
{
    levels = Clamp((int)levels, 1, MAX_LOD_LEVELS);
    if (levels != maxLodLevels_)
    {
        maxLodLevels_ = levels;
        lastPatchSize_ = 0; // Force full recreate

        CreateGeometry();
        MarkNetworkUpdate();
    }
}

void Terrain::SetOcclusionLodLevel(unsigned level)
{
    if (level != occlusionLodLevel_)
    {
        occlusionLodLevel_ = level;
        lastPatchSize_ = 0; // Force full recreate

        CreateGeometry();
        MarkNetworkUpdate();
    }
}

void Terrain::SetSmoothing(bool enable)
{
    if (enable != smoothing_)
    {
        smoothing_ = enable;

        CreateGeometry();
        MarkNetworkUpdate();
    }
}

bool Terrain::SetHeightMap(Image* image)
{
    bool success = SetHeightMapInternal(image, true);

    MarkNetworkUpdate();
    return success;
}

void Terrain::SetMaterial(Material* material)
{
    material_ = material;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetMaterial(material);
    }

    MarkNetworkUpdate();
}

void Terrain::SetDrawDistance(float distance)
{
    drawDistance_ = distance;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetDrawDistance(distance);
    }

    MarkNetworkUpdate();
}

void Terrain::SetShadowDistance(float distance)
{
    shadowDistance_ = distance;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetShadowDistance(distance);
    }

    MarkNetworkUpdate();
}

void Terrain::SetLodBias(float bias)
{
    lodBias_ = bias;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetLodBias(bias);
    }

    MarkNetworkUpdate();
}

void Terrain::SetViewMask(unsigned mask)
{
    viewMask_ = mask;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetViewMask(mask);
    }

    MarkNetworkUpdate();
}

void Terrain::SetLightMask(unsigned mask)
{
    lightMask_ = mask;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetLightMask(mask);
    }

    MarkNetworkUpdate();
}

void Terrain::SetShadowMask(unsigned mask)
{
    shadowMask_ = mask;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetShadowMask(mask);
    }

    MarkNetworkUpdate();
}

void Terrain::SetZoneMask(unsigned mask)
{
    zoneMask_ = mask;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetZoneMask(mask);
    }

    MarkNetworkUpdate();
}

void Terrain::SetMaxLights(unsigned num)
{
    maxLights_ = num;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetMaxLights(num);
    }

    MarkNetworkUpdate();
}

void Terrain::SetCastShadows(bool enable)
{
    castShadows_ = enable;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetCastShadows(enable);
    }

    MarkNetworkUpdate();
}

void Terrain::SetOccluder(bool enable)
{
    occluder_ = enable;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetOccluder(enable);
    }

    MarkNetworkUpdate();
}

void Terrain::SetOccludee(bool enable)
{
    occludee_ = enable;
    for (unsigned i = 0; i < patches_.size(); ++i)
    {
        if (patches_[i])
            patches_[i]->SetOccludee(enable);
    }

    MarkNetworkUpdate();
}

void Terrain::ApplyHeightMap()
{
    if (heightMap_)
        CreateGeometry();
}

Image* Terrain::GetHeightMap() const
{
    return heightMap_;
}

Material* Terrain::GetMaterial() const
{
    return material_;
}

TerrainPatch* Terrain::GetPatch(unsigned index) const
{
    return index < patches_.size() ? patches_[index] : (TerrainPatch*)nullptr;
}

TerrainPatch* Terrain::GetPatch(int x, int z) const
{
    if (x < 0 || x >= numPatches_.x_ || z < 0 || z >= numPatches_.y_)
        return nullptr;
    else
        return GetPatch(z * numPatches_.x_ + x);
}

float Terrain::GetHeight(const Vector3& worldPosition) const
{
    if (node_)
    {
        Vector3 position = node_->GetWorldTransform().Inverse() * worldPosition;
        float xPos = (position.x_ - patchWorldOrigin_.x_) / spacing_.x_;
        float zPos = (position.z_ - patchWorldOrigin_.y_) / spacing_.z_;
        float xFrac = xPos - floorf(xPos);
        float zFrac = zPos - floorf(zPos);
        float h1, h2, h3;

        if (xFrac + zFrac >= 1.0f)
        {
            h1 = GetRawHeight((unsigned)xPos + 1, (unsigned)zPos + 1);
            h2 = GetRawHeight((unsigned)xPos, (unsigned)zPos + 1);
            h3 = GetRawHeight((unsigned)xPos + 1, (unsigned)zPos);
            xFrac = 1.0f - xFrac;
            zFrac = 1.0f - zFrac;
        }
        else
        {
            h1 = GetRawHeight((unsigned)xPos, (unsigned)zPos);
            h2 = GetRawHeight((unsigned)xPos + 1, (unsigned)zPos);
            h3 = GetRawHeight((unsigned)xPos, (unsigned)zPos + 1);
        }

        float h = h1 * (1.0f - xFrac - zFrac) + h2 * xFrac + h3 * zFrac;
        /// \todo This assumes that the terrain scene node is upright
        return node_->GetWorldScale().y_ * h + node_->GetWorldPosition().y_;
    }
    else
        return 0.0f;
}

Vector3 Terrain::GetNormal(const Vector3& worldPosition) const
{
    if (node_)
    {
        Vector3 position = node_->GetWorldTransform().Inverse() * worldPosition;
        float xPos = (position.x_ - patchWorldOrigin_.x_) / spacing_.x_;
        float zPos = (position.z_ - patchWorldOrigin_.y_) / spacing_.z_;
        float xFrac = xPos - floorf(xPos);
        float zFrac = zPos - floorf(zPos);
        Vector3 n1, n2, n3;

        if (xFrac + zFrac >= 1.0f)
        {
            n1 = GetRawNormal((unsigned)xPos + 1, (unsigned)zPos + 1);
            n2 = GetRawNormal((unsigned)xPos, (unsigned)zPos + 1);
            n3 = GetRawNormal((unsigned)xPos + 1, (unsigned)zPos);
            xFrac = 1.0f - xFrac;
            zFrac = 1.0f - zFrac;
        }
        else
        {
            n1 = GetRawNormal((unsigned)xPos, (unsigned)zPos);
            n2 = GetRawNormal((unsigned)xPos + 1, (unsigned)zPos);
            n3 = GetRawNormal((unsigned)xPos, (unsigned)zPos + 1);
        }

        Vector3 n = (n1 * (1.0f - xFrac - zFrac) + n2 * xFrac + n3 * zFrac).Normalized();
        return node_->GetWorldRotation() * n;
    }
    else
        return Vector3::UP;
}

IntVector2 Terrain::WorldToHeightMap(const Vector3& worldPosition) const
{
    if (!node_)
        return IntVector2::ZERO;

    Vector3 position = node_->GetWorldTransform().Inverse() * worldPosition;
    int xPos = (int)((position.x_ - patchWorldOrigin_.x_) / spacing_.x_);
    int zPos = (int)((position.z_ - patchWorldOrigin_.y_) / spacing_.z_);
    Clamp(xPos, 0, numVertices_.x_);
    Clamp(zPos, 0, numVertices_.y_);

    return IntVector2(xPos, numVertices_.y_ - zPos);
}

void Terrain::CreatePatchGeometry(TerrainPatch* patch)
{
    URHO3D_PROFILE(CreatePatchGeometry);

    unsigned row = patchSize_ + 1;
    VertexBuffer* vertexBuffer = patch->GetVertexBuffer();
    Geometry* geometry = patch->GetGeometry();
    Geometry* maxLodGeometry = patch->GetMaxLodGeometry();
    Geometry* occlusionGeometry = patch->GetOcclusionGeometry();

    if (vertexBuffer->GetVertexCount() != row * row)
        vertexBuffer->SetSize(row * row, MASK_POSITION | MASK_NORMAL | MASK_TEXCOORD1 | MASK_TANGENT);

    SharedArrayPtr<unsigned char> cpuVertexData(new unsigned char[row * row * sizeof(Vector3)]);
    SharedArrayPtr<unsigned char> occlusionCpuVertexData(new unsigned char[row * row * sizeof(Vector3)]);

    float* vertexData = (float*)vertexBuffer->Lock(0, vertexBuffer->GetVertexCount());
    float* positionData = (float*)cpuVertexData.Get();
    float* occlusionData = (float*)occlusionCpuVertexData.Get();
    BoundingBox box;

    unsigned occlusionLevel = occlusionLodLevel_;
    if (occlusionLevel > numLodLevels_ - 1)
        occlusionLevel = numLodLevels_ - 1;

    if (vertexData)
    {
        const IntVector2& coords = patch->GetCoordinates();

        int lodExpand = (1 << (occlusionLevel)) - 1;
        int halfLodExpand = (1 << (occlusionLevel)) / 2;

        for (int z = 0; z <= patchSize_; ++z)
        {
            for (int x = 0; x <= patchSize_; ++x)
            {
                int xPos = coords.x_ * patchSize_ + x;
                int zPos = coords.y_ * patchSize_ + z;

                // Position
                Vector3 position((float)x * spacing_.x_, GetRawHeight(xPos, zPos), (float)z * spacing_.z_);
                *vertexData++ = position.x_;
                *vertexData++ = position.y_;
                *vertexData++ = position.z_;
                *positionData++ = position.x_;
                *positionData++ = position.y_;
                *positionData++ = position.z_;

                box.Merge(position);

                // For vertices that are part of the occlusion LOD, calculate the minimum height in the neighborhood
                // to prevent false positive occlusion due to inaccuracy between occlusion LOD & visible LOD
                float minHeight = position.y_;
                if (halfLodExpand > 0 && (x & lodExpand) == 0 && (z & lodExpand) == 0)
                {
                    int minX = Max(xPos - halfLodExpand, 0);
                    int maxX = Min(xPos + halfLodExpand, numVertices_.x_ - 1);
                    int minZ = Max(zPos - halfLodExpand, 0);
                    int maxZ = Min(zPos + halfLodExpand, numVertices_.y_ - 1);
                    for (int nZ = minZ; nZ <= maxZ; ++nZ)
                    {
                        for (int nX = minX; nX <= maxX; ++nX)
                            minHeight = Min(minHeight, GetRawHeight(nX, nZ));
                    }
                }
                *occlusionData++ = position.x_;
                *occlusionData++ = minHeight;
                *occlusionData++ = position.z_;
                // Normal
                Vector3 normal = GetRawNormal(xPos, zPos);
                *vertexData++ = normal.x_;
                *vertexData++ = normal.y_;
                *vertexData++ = normal.z_;

                // Texture coordinate
                Vector2 texCoord((float)xPos / (float)numVertices_.x_, 1.0f - (float)zPos / (float)numVertices_.y_);
                *vertexData++ = texCoord.x_;
                *vertexData++ = texCoord.y_;

                // Tangent
                Vector3 xyz = (Vector3::RIGHT - normal * normal.DotProduct(Vector3::RIGHT)).Normalized();
                *vertexData++ = xyz.x_;
                *vertexData++ = xyz.y_;
                *vertexData++ = xyz.z_;
                *vertexData++ = 1.0f;
            }
        }

        vertexBuffer->Unlock();
        vertexBuffer->ClearDataLost();
    }

    patch->SetBoundingBox(box);

    if (drawRanges_.size())
    {
        unsigned occlusionDrawRange = occlusionLevel << 4;

        geometry->SetIndexBuffer(indexBuffer_);
        geometry->SetDrawRange(TRIANGLE_LIST, drawRanges_[0].first, drawRanges_[0].second, false);
        geometry->SetRawVertexData(cpuVertexData, sizeof(Vector3), MASK_POSITION);
        maxLodGeometry->SetIndexBuffer(indexBuffer_);
        maxLodGeometry->SetDrawRange(TRIANGLE_LIST, drawRanges_[0].first, drawRanges_[0].second, false);
        maxLodGeometry->SetRawVertexData(cpuVertexData, sizeof(Vector3), MASK_POSITION);
        occlusionGeometry->SetIndexBuffer(indexBuffer_);
        occlusionGeometry->SetDrawRange(TRIANGLE_LIST, drawRanges_[occlusionDrawRange].first, drawRanges_[occlusionDrawRange].second, false);
        occlusionGeometry->SetRawVertexData(occlusionCpuVertexData, sizeof(Vector3), MASK_POSITION);
    }

    patch->ResetLod();
}

void Terrain::UpdatePatchLod(TerrainPatch* patch)
{
    Geometry* geometry = patch->GetGeometry();

    // All LOD levels except the coarsest have 16 versions for stitching
    unsigned lodLevel = patch->GetLodLevel();
    unsigned drawRangeIndex = lodLevel << 4;
    if (lodLevel < numLodLevels_ - 1)
    {
        TerrainPatch* north = patch->GetNorthPatch();
        TerrainPatch* south = patch->GetSouthPatch();
        TerrainPatch* west = patch->GetWestPatch();
        TerrainPatch* east = patch->GetEastPatch();

        if (north && north->GetLodLevel() > lodLevel)
            drawRangeIndex |= STITCH_NORTH;
        if (south && south->GetLodLevel() > lodLevel)
            drawRangeIndex |= STITCH_SOUTH;
        if (west && west->GetLodLevel() > lodLevel)
            drawRangeIndex |= STITCH_WEST;
        if (east && east->GetLodLevel() > lodLevel)
            drawRangeIndex |= STITCH_EAST;
    }

    if (drawRangeIndex < drawRanges_.size())
        geometry->SetDrawRange(TRIANGLE_LIST, drawRanges_[drawRangeIndex].first, drawRanges_[drawRangeIndex].second, false);
}

void Terrain::SetMaterialAttr(const ResourceRef& value)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    SetMaterial(cache->GetResource<Material>(value.name_));
}

void Terrain::SetHeightMapAttr(const ResourceRef& value)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    Image* image = cache->GetResource<Image>(value.name_);
    SetHeightMapInternal(image, false);
}

void Terrain::SetPatchSizeAttr(int value)
{
    if (value < MIN_PATCH_SIZE || value > MAX_PATCH_SIZE || !IsPowerOfTwo(value))
        return;

    if (value != patchSize_)
    {
        patchSize_ = value;
        recreateTerrain_ = true;
    }
}

void Terrain::SetMaxLodLevelsAttr(unsigned value)
{
    value = Clamp((int)value, 1, MAX_LOD_LEVELS);

    if (value != maxLodLevels_)
    {
        maxLodLevels_ = value;
        lastPatchSize_ = 0; // Force full recreate
        recreateTerrain_ = true;
    }
}

void Terrain::SetOcclusionLodLevelAttr(unsigned value)
{
    if (value != occlusionLodLevel_)
    {
        occlusionLodLevel_ = value;
        lastPatchSize_ = 0; // Force full recreate
        recreateTerrain_ = true;
    }
}

ResourceRef Terrain::GetMaterialAttr() const
{
    return GetResourceRef(material_, Material::GetTypeStatic());
}

ResourceRef Terrain::GetHeightMapAttr() const
{
    return GetResourceRef(heightMap_, Image::GetTypeStatic());
}

void Terrain::CreateGeometry()
{
    recreateTerrain_ = false;

    if (!node_)
        return;

    URHO3D_PROFILE(CreateTerrainGeometry);

    unsigned prevNumPatches = patches_.size();

    // Determine number of LOD levels
    unsigned lodSize = patchSize_;
    numLodLevels_ = 1;
    while (lodSize > MIN_PATCH_SIZE && numLodLevels_ < maxLodLevels_)
    {
        lodSize >>= 1;
        ++numLodLevels_;
    }

    // Determine total terrain size
    patchWorldSize_ = Vector2(spacing_.x_ * (float)patchSize_, spacing_.z_ * (float)patchSize_);
    bool updateAll = false;

    if (heightMap_)
    {
        numPatches_ = IntVector2((heightMap_->GetWidth() - 1) / patchSize_, (heightMap_->GetHeight() - 1) / patchSize_);
        numVertices_ = IntVector2(numPatches_.x_ * patchSize_ + 1, numPatches_.y_ * patchSize_ + 1);
        patchWorldOrigin_ = Vector2(-0.5f * (float)numPatches_.x_ * patchWorldSize_.x_, -0.5f * (float)numPatches_.y_ *
                                    patchWorldSize_.y_);
        if (numVertices_ != lastNumVertices_ || lastSpacing_ != spacing_ || patchSize_ != lastPatchSize_ )
            updateAll = true;
        unsigned newDataSize = numVertices_.x_ * numVertices_.y_;

        // Create new height data if terrain size changed
        if (!heightData_ || updateAll)
            heightData_ = new float[newDataSize];

        // Ensure that the source (unsmoothed) data exists if smoothing is active
        if (smoothing_ && (!sourceHeightData_ || updateAll))
        {
            sourceHeightData_ = new float[newDataSize];
            updateAll = true;
        }
        else if (!smoothing_)
            sourceHeightData_.Reset();
    }
    else
    {
        numPatches_ = IntVector2::ZERO;
        numVertices_ = IntVector2::ZERO;
        patchWorldOrigin_ = Vector2::ZERO;
        heightData_.Reset();
        sourceHeightData_.Reset();
    }

    lastNumVertices_ = numVertices_;
    lastPatchSize_ = patchSize_;
    lastSpacing_ = spacing_;

    // Remove old patch nodes which are not needed
    if (updateAll)
    {
        URHO3D_PROFILE(RemoveOldPatches);

        std::vector<Node*> oldPatchNodes;
        node_->GetChildrenWithComponent<TerrainPatch>(oldPatchNodes);
        for (auto & oldPatchNode : oldPatchNodes)
        {
            bool nodeOk = false;
            QStringList coords = (oldPatchNode)->GetName().mid(6).split('_');
            if (coords.size() == 2)
            {
                int x = coords[0].toInt();
                int z = coords[1].toInt();
                if (x < numPatches_.x_ && z < numPatches_.y_)
                    nodeOk = true;
            }

            if (!nodeOk)
                node_->RemoveChild(oldPatchNode);
        }
    }

    // Keep track of which patches actually need an update
    std::vector<bool> dirtyPatches(numPatches_.x_ * numPatches_.y_);
    for (unsigned i = 0; i < dirtyPatches.size(); ++i)
        dirtyPatches[i] = updateAll;

    patches_.clear();

    if (heightMap_)
    {
        // Copy heightmap data
        const unsigned char* src = heightMap_->GetData();
        float* dest = smoothing_ ? sourceHeightData_ : heightData_;
        unsigned imgComps = heightMap_->GetComponents();
        unsigned imgRow = heightMap_->GetWidth() * imgComps;
        IntRect updateRegion(-1, -1, -1, -1);

        if (imgComps == 1)
        {
            URHO3D_PROFILE(CopyHeightData);

            for (int z = 0; z < numVertices_.y_; ++z)
            {
                for (int x = 0; x < numVertices_.x_; ++x)
                {
                    float newHeight = (float)src[imgRow * (numVertices_.y_ - 1 - z) + x] * spacing_.y_;

                    if (updateAll)
                        *dest = newHeight;
                    else
                    {
                        if (*dest != newHeight)
                        {

                            *dest = newHeight;
                            GrowUpdateRegion(updateRegion, x, z);
                        }
                    }

                    ++dest;
                }
            }
        }
        else
        {
            URHO3D_PROFILE(CopyHeightData);

            // If more than 1 component, use the green channel for more accuracy
            for (int z = 0; z < numVertices_.y_; ++z)
            {
                for (int x = 0; x < numVertices_.x_; ++x)
                {
                    float newHeight = ((float)src[imgRow * (numVertices_.y_ - 1 - z) + imgComps * x] + (float)src[imgRow *
                            (numVertices_.y_ - 1 - z) + imgComps * x + 1] / 256.0f) * spacing_.y_;

                    if (updateAll)
                        *dest = newHeight;
                    else
                    {
                        if (*dest != newHeight)
                        {

                            *dest = newHeight;
                            GrowUpdateRegion(updateRegion, x, z);
                        }
                    }

                    ++dest;
                }
            }
        }

        // If updating a region of the heightmap, check which patches change
        if (!updateAll)
        {
            int lodExpand = 1 << (numLodLevels_ - 1);
            // Expand the right & bottom 1 pixel more, as patches share vertices at the edge
            updateRegion.left_ -= lodExpand;
            updateRegion.right_ += lodExpand + 1;
            updateRegion.top_ -= lodExpand;
            updateRegion.bottom_ += lodExpand + 1;

            int sX = Max(updateRegion.left_ / patchSize_, 0);
            int eX = Min(updateRegion.right_ / patchSize_, numPatches_.x_ - 1);
            int sY = Max(updateRegion.top_ / patchSize_, 0);
            int eY = Min(updateRegion.bottom_ / patchSize_, numPatches_.y_ - 1);
            for (int y = sY; y <= eY; ++y)
            {
                for (int x = sX; x <= eX; ++x)
                    dirtyPatches[y * numPatches_.x_ + x] = true;
            }
        }
        patches_.reserve(numPatches_.x_ * numPatches_.y_);

        bool enabled = IsEnabledEffective();

        {
            URHO3D_PROFILE(CreatePatches);

            // Create patches and set node transforms
            for (int z = 0; z < numPatches_.y_; ++z)
            {
                for (int x = 0; x < numPatches_.x_; ++x)
                {
                    QString nodeName = "Patch_" + QString::number(x) + "_" + QString::number(z);
                    Node* patchNode = node_->GetChild(nodeName);

                    if (!patchNode)
                    {
                        // Create the patch scene node as local and temporary so that it is not unnecessarily serialized to either
                        // file or replicated over the network
                        patchNode = node_->CreateChild(nodeName, LOCAL);
                        patchNode->SetTemporary(true);
                    }

                    patchNode->SetPosition(Vector3(patchWorldOrigin_.x_ + (float)x * patchWorldSize_.x_, 0.0f, patchWorldOrigin_.y_ +
                                                   (float)z * patchWorldSize_.y_));

                    TerrainPatch* patch = patchNode->GetComponent<TerrainPatch>();
                    if (!patch)
                    {
                        patch = patchNode->CreateComponent<TerrainPatch>();
                        patch->SetOwner(this);
                        patch->SetCoordinates(IntVector2(x, z));

                        // Copy initial drawable parameters
                        patch->SetEnabled(enabled);
                        patch->SetMaterial(material_);
                        patch->SetDrawDistance(drawDistance_);
                        patch->SetShadowDistance(shadowDistance_);
                        patch->SetLodBias(lodBias_);
                        patch->SetViewMask(viewMask_);
                        patch->SetLightMask(lightMask_);
                        patch->SetShadowMask(shadowMask_);
                        patch->SetZoneMask(zoneMask_);
                        patch->SetMaxLights(maxLights_);
                        patch->SetCastShadows(castShadows_);
                        patch->SetOccluder(occluder_);
                        patch->SetOccludee(occludee_);
                    }

                    patches_.push_back(WeakPtr<TerrainPatch>(patch));
                }
            }
        }

        // Create the shared index data
        if (updateAll)
            CreateIndexData();

        // Create vertex data for patches. First update smoothing to ensure normals are calculated correctly across patch borders
        if (smoothing_)
        {
            URHO3D_PROFILE(UpdateSmoothing);

            for (unsigned i = 0; i < patches_.size(); ++i)
            {
                if (dirtyPatches[i])
                {
                    TerrainPatch* patch = patches_[i];
                    const IntVector2& coords = patch->GetCoordinates();
                    int startX = coords.x_ * patchSize_;
                    int endX = startX + patchSize_;
                    int startZ = coords.y_ * patchSize_;
                    int endZ = startZ + patchSize_;

                    for (int z = startZ; z <= endZ; ++z)
                    {
                        for (int x = startX; x <= endX; ++x)
                        {
                            float smoothedHeight = (
                                        GetSourceHeight(x - 1, z - 1) + GetSourceHeight(x, z - 1) * 2.0f + GetSourceHeight(x + 1, z - 1) +
                                        GetSourceHeight(x - 1, z) * 2.0f + GetSourceHeight(x, z) * 4.0f + GetSourceHeight(x + 1, z) * 2.0f +
                                        GetSourceHeight(x - 1, z + 1) + GetSourceHeight(x, z + 1) * 2.0f + GetSourceHeight(x + 1, z + 1)
                                        ) / 16.0f;

                            heightData_[z * numVertices_.x_ + x] = smoothedHeight;
                        }
                    }
                }
            }
        }

        for (unsigned i = 0; i < patches_.size(); ++i)
        {
            TerrainPatch* patch = patches_[i];

            if (dirtyPatches[i])
            {
                CreatePatchGeometry(patch);
                CalculateLodErrors(patch);
            }

            SetNeighbors(patch);
        }
    }

    // Send event only if new geometry was generated, or the old was cleared
    if (patches_.size() || prevNumPatches)
    {
        using namespace TerrainCreated;

        VariantMap& eventData = GetEventDataMap();
        eventData[P_NODE] = node_;
        node_->SendEvent(E_TERRAINCREATED, eventData);
    }
}

void Terrain::CreateIndexData()
{
    URHO3D_PROFILE(CreateIndexData);

    std::vector<unsigned short> indices;
    drawRanges_.clear();
    unsigned row = patchSize_ + 1;

    /* Build index data for each LOD level. Each LOD level except the lowest can stitch to the next lower LOD from the edges:
       north, south, west, east, or any combination of them, requiring 16 different versions of each LOD level's index data

       Normal edge:     Stitched edge:
       +----+----+      +---------+
       |\   |\   |      |\       /|
       | \  | \  |      | \     / |
       |  \ |  \ |      |  \   /  |
       |   \|   \|      |   \ /   |
       +----+----+      +----+----+
    */
    for (unsigned i = 0; i < numLodLevels_; ++i)
    {
        unsigned combinations = (i < numLodLevels_ - 1) ? 16 : 1;
        int skip = 1 << i;

        for (unsigned j = 0; j < combinations; ++j)
        {
            unsigned indexStart = indices.size();

            int zStart = 0;
            int xStart = 0;
            int zEnd = patchSize_;
            int xEnd = patchSize_;

            if (j & STITCH_NORTH)
                zEnd -= skip;
            if (j & STITCH_SOUTH)
                zStart += skip;
            if (j & STITCH_WEST)
                xStart += skip;
            if (j & STITCH_EAST)
                xEnd -= skip;

            // Build the main grid
            for (int z = zStart; z < zEnd; z += skip)
            {
                for (int x = xStart; x < xEnd; x += skip)
                {
                    indices.push_back((z + skip) * row + x);
                    indices.push_back(z * row + x + skip);
                    indices.push_back(z * row + x);
                    indices.push_back((z + skip) * row + x);
                    indices.push_back((z + skip) * row + x + skip);
                    indices.push_back(z * row + x + skip);
                }
            }

            // Build the north edge
            if (j & STITCH_NORTH)
            {
                int z = patchSize_ - skip;
                for (int x = 0; x < patchSize_; x += skip * 2)
                {
                    if (x > 0 || (j & STITCH_WEST) == 0)
                    {
                        indices.push_back((z + skip) * row + x);
                        indices.push_back(z * row + x + skip);
                        indices.push_back(z * row + x);
                    }
                    indices.push_back((z + skip) * row + x);
                    indices.push_back((z + skip) * row + x + 2 * skip);
                    indices.push_back(z * row + x + skip);
                    if (x < patchSize_ - skip * 2 || (j & STITCH_EAST) == 0)
                    {
                        indices.push_back((z + skip) * row + x + 2 * skip);
                        indices.push_back(z * row + x + 2 * skip);
                        indices.push_back(z * row + x + skip);
                    }
                }
            }

            // Build the south edge
            if (j & STITCH_SOUTH)
            {
                int z = 0;
                for (int x = 0; x < patchSize_; x += skip * 2)
                {
                    if (x > 0 || (j & STITCH_WEST) == 0)
                    {
                        indices.push_back((z + skip) * row + x);
                        indices.push_back((z + skip) * row + x + skip);
                        indices.push_back(z * row + x);
                    }
                    indices.push_back(z * row + x);
                    indices.push_back((z + skip) * row + x + skip);
                    indices.push_back(z * row + x + 2 * skip);
                    if (x < patchSize_ - skip * 2 || (j & STITCH_EAST) == 0)
                    {
                        indices.push_back((z + skip) * row + x + skip);
                        indices.push_back((z + skip) * row + x + 2 * skip);
                        indices.push_back(z * row + x + 2 * skip);
                    }
                }
            }

            // Build the west edge
            if (j & STITCH_WEST)
            {
                int x = 0;
                for (int z = 0; z < patchSize_; z += skip * 2)
                {
                    if (z > 0 || (j & STITCH_SOUTH) == 0)
                    {
                        indices.push_back(z * row + x);
                        indices.push_back((z + skip) * row + x + skip);
                        indices.push_back(z * row + x + skip);
                    }
                    indices.push_back((z + 2 * skip) * row + x);
                    indices.push_back((z + skip) * row + x + skip);
                    indices.push_back(z * row + x);
                    if (x < patchSize_ - skip * 2 || (j & STITCH_NORTH) == 0)
                    {
                        indices.push_back((z + 2 * skip) * row + x);
                        indices.push_back((z + 2 * skip) * row + x + skip);
                        indices.push_back((z + skip) * row + x + skip);
                    }
                }
            }

            // Build the east edge
            if (j & STITCH_EAST)
            {
                int x = patchSize_ - skip;
                for (int z = 0; z < patchSize_; z += skip * 2)
                {
                    if (z > 0 || (j & STITCH_SOUTH) == 0)
                    {
                        indices.push_back(z * row + x);
                        indices.push_back((z + skip) * row + x);
                        indices.push_back(z * row + x + skip);
                    }
                    indices.push_back((z + skip) * row + x);
                    indices.push_back((z + 2 * skip) * row + x + skip);
                    indices.push_back(z * row + x + skip);
                    if (z < patchSize_ - skip * 2 || (j & STITCH_NORTH) == 0)
                    {
                        indices.push_back((z + skip) * row + x);
                        indices.push_back((z + 2 * skip) * row + x);
                        indices.push_back((z + 2 * skip) * row + x + skip);
                    }
                }
            }

            drawRanges_.emplace_back(indexStart, indices.size() - indexStart);
        }
    }

    indexBuffer_->SetSize(indices.size(), false);
    indexBuffer_->SetData(&indices[0]);
}

float Terrain::GetRawHeight(int x, int z) const
{
    if (!heightData_)
        return 0.0f;

    x = Clamp(x, 0, numVertices_.x_ - 1);
    z = Clamp(z, 0, numVertices_.y_ - 1);
    return heightData_[z * numVertices_.x_ + x];
}

float Terrain::GetSourceHeight(int x, int z) const
{
    if (!sourceHeightData_)
        return 0.0f;

    x = Clamp(x, 0, numVertices_.x_ - 1);
    z = Clamp(z, 0, numVertices_.y_ - 1);
    return sourceHeightData_[z * numVertices_.x_ + x];
}

float Terrain::GetLodHeight(int x, int z, unsigned lodLevel) const
{
    unsigned offset = 1 << lodLevel;
    float divisor = (float)offset;
    float xFrac = (float)(x % offset) / divisor;
    float zFrac = (float)(z % offset) / divisor;
    float h1, h2, h3;

    if (xFrac + zFrac >= 1.0f)
    {
        h1 = GetRawHeight(x + offset, z + offset);
        h2 = GetRawHeight(x, z + offset);
        h3 = GetRawHeight(x + offset, z);
        xFrac = 1.0f - xFrac;
        zFrac = 1.0f - zFrac;
    }
    else
    {
        h1 = GetRawHeight(x, z);
        h2 = GetRawHeight(x + offset, z);
        h3 = GetRawHeight(x, z + offset);
    }

    return h1 * (1.0f - xFrac - zFrac) + h2 * xFrac + h3 * zFrac;
}

Vector3 Terrain::GetRawNormal(int x, int z) const
{
    float baseHeight = GetRawHeight(x, z);
    float nSlope = GetRawHeight(x, z - 1) - baseHeight;
    float neSlope = GetRawHeight(x + 1, z - 1) - baseHeight;
    float eSlope = GetRawHeight(x + 1, z) - baseHeight;
    float seSlope = GetRawHeight(x + 1, z + 1) - baseHeight;
    float sSlope = GetRawHeight(x, z + 1) - baseHeight;
    float swSlope = GetRawHeight(x - 1, z + 1) - baseHeight;
    float wSlope = GetRawHeight(x - 1, z) - baseHeight;
    float nwSlope = GetRawHeight(x - 1, z - 1) - baseHeight;
    float up = 0.5f * (spacing_.x_ + spacing_.z_);

    return (Vector3(0.0f, up, nSlope) +
            Vector3(-neSlope, up, neSlope) +
            Vector3(-eSlope, up, 0.0f) +
            Vector3(-seSlope, up, -seSlope) +
            Vector3(0.0f, up, -sSlope) +
            Vector3(swSlope, up, -swSlope) +
            Vector3(wSlope, up, 0.0f) +
            Vector3(nwSlope, up, nwSlope)).Normalized();
}

void Terrain::CalculateLodErrors(TerrainPatch* patch)
{
    URHO3D_PROFILE(CalculateLodErrors);

    const IntVector2& coords = patch->GetCoordinates();
    std::vector<float>& lodErrors = patch->GetLodErrors();
    lodErrors.clear();
    lodErrors.reserve(numLodLevels_);

    int xStart = coords.x_ * patchSize_;
    int zStart = coords.y_ * patchSize_;
    int xEnd = xStart + patchSize_;
    int zEnd = zStart + patchSize_;

    for (unsigned i = 0; i < numLodLevels_; ++i)
    {
        float maxError = 0.0f;
        int divisor = 1 << i;

        if (i > 0)
        {
            for (int z = zStart; z <= zEnd; ++z)
            {
                for (int x = xStart; x <= xEnd; ++x)
                {
                    if (x % divisor || z % divisor)
                    {
                        float error = Abs(GetLodHeight(x, z, i) - GetRawHeight(x, z));
                        maxError = Max(error, maxError);
                    }
                }
            }

            // Set error to be at least same as (half vertex spacing x LOD) to prevent horizontal stretches getting too inaccurate
            maxError = Max(maxError, 0.25f * (spacing_.x_ + spacing_.z_) * (float)(1 << i));
        }

        lodErrors.push_back(maxError);
    }
}

void Terrain::SetNeighbors(TerrainPatch* patch)
{
    const IntVector2& coords = patch->GetCoordinates();
    patch->SetNeighbors(GetPatch(coords.x_, coords.y_ + 1), GetPatch(coords.x_, coords.y_ - 1),
                        GetPatch(coords.x_ - 1, coords.y_), GetPatch(coords.x_ + 1, coords.y_));
}

bool Terrain::SetHeightMapInternal(Image* image, bool recreateNow)
{
    if (image && image->IsCompressed())
    {
        URHO3D_LOGERROR("Can not use a compressed image as a terrain heightmap");
        return false;
    }

    // Unsubscribe from the reload event of previous image (if any), then subscribe to the new
    if (heightMap_)
        UnsubscribeFromEvent(heightMap_, E_RELOADFINISHED);
    if (image)
        SubscribeToEvent(image, E_RELOADFINISHED, URHO3D_HANDLER(Terrain, HandleHeightMapReloadFinished));

    heightMap_ = image;

    if (recreateNow)
        CreateGeometry();
    else
        recreateTerrain_ = true;

    return true;
}

void Terrain::HandleHeightMapReloadFinished(StringHash eventType, VariantMap& eventData)
{
    CreateGeometry();
}

}