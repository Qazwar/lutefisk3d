//
// Copyright (c) 2008-2017 the Urho3D project.
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

#include "TmxFile2D.h"

#include "Sprite2D.h"

#include "Lutefisk3D/Core/Context.h"
#include "Lutefisk3D/IO/FileSystem.h"
#include "Lutefisk3D/IO/Log.h"
#include "Lutefisk3D/IO/File.h"
#include "Lutefisk3D/Resource/ResourceCache.h"
#include "Lutefisk3D/Graphics/Texture2D.h"
#include "Lutefisk3D/Graphics/Graphics.h"
#include "Lutefisk3D/Core/StringUtils.h"
#include "Lutefisk3D/Resource/XMLFile.h"
#include "Lutefisk3D/Resource/Image.h"
#include "Lutefisk3D/Math/AreaAllocator.h"


namespace Urho3D
{

extern const float PIXEL_SIZE;

TmxLayer2D::TmxLayer2D(TmxFile2D* tmxFile, TileMapLayerType2D type) :
    tmxFile_(tmxFile),
    type_(type)
{

}

TmxLayer2D::~TmxLayer2D()
{
}

TmxFile2D* TmxLayer2D::GetTmxFile() const
{
    return tmxFile_;
}

bool TmxLayer2D::HasProperty(const QString& name) const
{
    if (!propertySet_)
        return false;
    return propertySet_->HasProperty(name);
}

const QString& TmxLayer2D::GetProperty(const QString& name) const
{
    if (!propertySet_)
        return s_dummy;
    return propertySet_->GetProperty(name);
}

void TmxLayer2D::LoadInfo(const XMLElement& element)
{
    name_ = element.GetAttribute("name");
    width_ = element.GetInt("width");
    height_ = element.GetInt("height");
    if (element.HasAttribute("visible"))
        visible_ = element.GetInt("visible") != 0;
    else
        visible_ = true;
}

void TmxLayer2D::LoadPropertySet(const XMLElement& element)
{
    propertySet_ = new PropertySet2D();
    propertySet_->Load(element);
}

TmxTileLayer2D::TmxTileLayer2D(TmxFile2D* tmxFile) :
    TmxLayer2D(tmxFile, LT_TILE_LAYER)
{
}

enum LayerEncoding {
    XML,
    CSV,
    Base64,
};
bool TmxTileLayer2D::Load(const XMLElement& element, const TileMapInfo2D& info)
{
    LoadInfo(element);

    XMLElement dataElem = element.GetChild("data");
    if (!dataElem)
    {
        URHO3D_LOGERROR("Could not find data in layer");
        return false;
    }

    LayerEncoding encoding;
    if (dataElem.HasAttribute("compression"))
    {
        URHO3D_LOGERROR("Compression not supported now");
        return false;
    }

    if (dataElem.HasAttribute("encoding"))
    {
        QString encodingAttribute = dataElem.GetAttribute("encoding");
        if (encodingAttribute == "xml")
            encoding = XML;
        else if (encodingAttribute == "csv")
            encoding = CSV;
        else if (encodingAttribute == "base64")
            encoding = Base64;
        else
        {
            URHO3D_LOGERROR("Invalid encoding: " + encodingAttribute);
            return false;
        }
    }
    else
        encoding = XML;

    tiles_.resize((unsigned)(width_ * height_));
    if (encoding == XML)
    {
        XMLElement tileElem = dataElem.GetChild("tile");

        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                if (!tileElem)
                    return false;

                int gid = tileElem.GetInt("gid");
                if (gid > 0)
                {
                    SharedPtr<Tile2D> tile(new Tile2D());
                    tile->gid_ = gid;
                    tile->sprite_ = tmxFile_->GetTileSprite(gid);
                    tile->propertySet_ = tmxFile_->GetTilePropertySet(gid);
                    tiles_[y * width_ + x] = tile;
                }

                tileElem = tileElem.GetNext("tile");
            }
        }
    }
    else if (encoding == CSV)
    {
        QString dataValue = dataElem.GetValue();
        auto gidVector = dataValue.split(',');
        int currentIndex = 0;
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                gidVector[currentIndex].replace("\n", "");
                int gid = gidVector[currentIndex].toInt();
                if (gid > 0)
                {
                    SharedPtr<Tile2D> tile(new Tile2D());
                    tile->gid_ = gid;
                    tile->sprite_ = tmxFile_->GetTileSprite(gid);
                    tile->propertySet_ = tmxFile_->GetTilePropertySet(gid);
                    tiles_[y * width_ + x] = tile;
                }
                ++currentIndex;
            }
        }
    }
    else if (encoding == Base64)
    {
        QString dataValue = dataElem.GetValue();
        int startPosition = 0;
        while (!dataValue[startPosition].isLetterOrNumber()
               && dataValue[startPosition] != '+' && dataValue[startPosition] != '/')
            ++startPosition;
        dataValue = dataValue.mid(startPosition);

        QByteArray buffer(QByteArray::fromBase64(dataValue.toLatin1()));
        int currentIndex = 0;
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                // buffer contains 32-bit integers in little-endian format
                int gid = (buffer[currentIndex+3] << 24) | (buffer[currentIndex+2] << 16)
                        | (buffer[currentIndex+1] << 8) | buffer[currentIndex];
                if (gid > 0)
                {
                    SharedPtr<Tile2D> tile(new Tile2D());
                    tile->gid_ = gid;
                    tile->sprite_ = tmxFile_->GetTileSprite(gid);
                    tile->propertySet_ = tmxFile_->GetTilePropertySet(gid);
                    tiles_[y * width_ + x] = tile;
                }
                currentIndex += 4;
            }
        }
    }

    if (element.HasChild("properties"))
        LoadPropertySet(element.GetChild("properties"));

    return true;
}

Tile2D* TmxTileLayer2D::GetTile(int x, int y) const
{
    if (x < 0 || x >= width_ || y < 0 || y >= height_)
        return nullptr;

    return tiles_[y * width_ + x];
}

TmxObjectGroup2D::TmxObjectGroup2D(TmxFile2D* tmxFile) :
    TmxLayer2D(tmxFile, LT_OBJECT_GROUP)
{
}

bool TmxObjectGroup2D::Load(const XMLElement &element, const TileMapInfo2D &info)
{
    LoadInfo(element);

    for (XMLElement objectElem = element.GetChild("object"); objectElem; objectElem = objectElem.GetNext("object"))
    {
        SharedPtr<TileMapObject2D> object(new TileMapObject2D());
        StoreObject(objectElem, object, info);
        objects_.push_back(object);
    }

    if (element.HasChild("properties"))
        LoadPropertySet(element.GetChild("properties"));

    return true;
}

void TmxObjectGroup2D::StoreObject(XMLElement objectElem, SharedPtr<TileMapObject2D> object, const TileMapInfo2D& info, bool isTile)
{
    if (objectElem.HasAttribute("name"))
        object->name_ = objectElem.GetAttribute("name");
    if (objectElem.HasAttribute("type"))
        object->type_ = objectElem.GetAttribute("type");

    if (objectElem.HasAttribute("gid"))
        object->objectType_ = OT_TILE;
    else if (objectElem.HasChild("polygon"))
        object->objectType_ = OT_POLYGON;
    else if (objectElem.HasChild("polyline"))
        object->objectType_ = OT_POLYLINE;
    else if (objectElem.HasChild("ellipse"))
        object->objectType_ = OT_ELLIPSE;
    else
        object->objectType_ = OT_RECTANGLE;

    const Vector2 position(objectElem.GetFloat("x"), objectElem.GetFloat("y"));
    const Vector2 size(objectElem.GetFloat("width"), objectElem.GetFloat("height"));

    switch (object->objectType_)
    {
        case OT_RECTANGLE:
        case OT_ELLIPSE:
            object->position_ = info.ConvertPosition(Vector2(position.x_, position.y_ + size.y_));
            object->size_     = Vector2(size.x_ * PIXEL_SIZE, size.y_ * PIXEL_SIZE);
            break;

        case OT_TILE:
            object->position_ = info.ConvertPosition(position);
            object->gid_      = objectElem.GetInt("gid");
            object->sprite_   = tmxFile_->GetTileSprite(object->gid_);

            if (objectElem.HasAttribute("width") || objectElem.HasAttribute("height"))
            {
                object->size_ = Vector2(size.x_ * PIXEL_SIZE, size.y_ * PIXEL_SIZE);
            }
            else if (object->sprite_)
            {
                IntVector2 spriteSize = object->sprite_->GetRectangle().Size();
                object->size_         = Vector2(spriteSize.x_, spriteSize.y_);
            }

            break;

        case OT_POLYGON:
        case OT_POLYLINE:
        {
            QStringList points;

            const char *name        = object->objectType_ == OT_POLYGON ? "polygon" : "polyline";
            XMLElement  polygonElem = objectElem.GetChild(name);
            points                  = polygonElem.GetAttribute("points").split(' ');

            if (points.size() <= 1)
                return;

            object->points_.resize(points.size());

            for (unsigned i = 0; i < points.size(); ++i)
            {
                points[i].replace(',', ' ');
                Vector2 point      = position + ToVector2(points[i]);
                object->points_[i] = info.ConvertPosition(point);
            }
        }
            break;

        default: break;
    }

    if (objectElem.HasChild("properties"))
    {
        object->propertySet_ = new PropertySet2D();
        object->propertySet_->Load(objectElem.GetChild("properties"));
    }

}

TileMapObject2D* TmxObjectGroup2D::GetObject(unsigned index) const
{
    if (index >= objects_.size())
        return nullptr;
    return objects_[index];
}


TmxImageLayer2D::TmxImageLayer2D(TmxFile2D* tmxFile) :
    TmxLayer2D(tmxFile, LT_IMAGE_LAYER)
{
}

bool TmxImageLayer2D::Load(const XMLElement& element, const TileMapInfo2D& info)
{
    LoadInfo(element);

    XMLElement imageElem = element.GetChild("image");
    if (!imageElem)
        return false;

    position_ = Vector2(0.0f, info.GetMapHeight());
    source_ = imageElem.GetAttribute("source");
    QString textureFilePath = GetParentPath(tmxFile_->GetName()) + source_;
    ResourceCache* cache = tmxFile_->GetContext()->m_ResourceCache.get();
    SharedPtr<Texture2D> texture(cache->GetResource<Texture2D>(textureFilePath));
    if (!texture)
    {
        URHO3D_LOGERROR("Could not load texture " + textureFilePath);
        return false;
    }

    sprite_ = new Sprite2D(tmxFile_->GetContext());
    sprite_->SetTexture(texture);
    sprite_->SetRectangle(IntRect(0, 0, texture->GetWidth(), texture->GetHeight()));
    // Set image hot spot at left top
    sprite_->SetHotSpot(Vector2(0.0f, 1.0f));

    if (element.HasChild("properties"))
        LoadPropertySet(element.GetChild("properties"));

    return true;
}

Sprite2D* TmxImageLayer2D::GetSprite() const
{
    return sprite_;
}

TmxFile2D::TmxFile2D(Context* context) :
    Resource(context)
{
}

TmxFile2D::~TmxFile2D()
{
    for (unsigned i = 0; i < layers_.size(); ++i)
        delete layers_[i];
}

void TmxFile2D::RegisterObject(Context* context)
{
    context->RegisterFactory<TmxFile2D>();
}

bool TmxFile2D::BeginLoad(Deserializer& source)
{
    if (GetName().isEmpty())
        SetName(source.GetName());

    loadXMLFile_ = new XMLFile(context_);
    if (!loadXMLFile_->Load(source))
    {
        URHO3D_LOGERROR("Load XML failed " + source.GetName());
        loadXMLFile_.Reset();
        return false;
    }

    XMLElement rootElem = loadXMLFile_->GetRoot("map");
    if (!rootElem)
    {
        URHO3D_LOGERROR("Invalid tmx file " + source.GetName());
        loadXMLFile_.Reset();
        return false;
    }

    // If we're async loading, request the texture now. Finish during EndLoad().
    if (GetAsyncLoadState() == ASYNC_LOADING)
    {
        for (XMLElement tileSetElem = rootElem.GetChild("tileset"); tileSetElem; tileSetElem = tileSetElem.GetNext("tileset"))
        {
            // Tile set defined in TSX file
            if (tileSetElem.HasAttribute("source"))
            {
                QString source = tileSetElem.GetAttribute("source");
                SharedPtr<XMLFile> tsxXMLFile = LoadTSXFile(source);
                if (!tsxXMLFile)
                    return false;

                tsxXMLFiles_[source] = tsxXMLFile;

                QString textureFilePath = GetParentPath(GetName()) + tsxXMLFile->GetRoot("tileset").GetChild("image").GetAttribute("source");
                context_->m_ResourceCache->BackgroundLoadResource<Texture2D>(textureFilePath, true, this);
            }
            else
            {
                QString textureFilePath = GetParentPath(GetName()) + tileSetElem.GetChild("image").GetAttribute("source");
                context_->m_ResourceCache->BackgroundLoadResource<Texture2D>(textureFilePath, true, this);
            }
        }

        for (XMLElement imageLayerElem = rootElem.GetChild("imagelayer"); imageLayerElem;
             imageLayerElem = imageLayerElem.GetNext("imagelayer"))
        {
            QString textureFilePath = GetParentPath(GetName()) + imageLayerElem.GetChild("image").GetAttribute("source");
            context_->m_ResourceCache->BackgroundLoadResource<Texture2D>(textureFilePath, true, this);
        }
    }

    return true;
}

bool TmxFile2D::EndLoad()
{
    if (!loadXMLFile_)
        return false;

    XMLElement rootElem = loadXMLFile_->GetRoot("map");
    QString version = rootElem.GetAttribute("version");
    if (version != "1.0")
    {
        URHO3D_LOGERROR("Invalid version");
        return false;
    }

    QString orientation = rootElem.GetAttribute("orientation");
    if (orientation == "orthogonal")
        info_.orientation_ = O_ORTHOGONAL;
    else if (orientation == "isometric")
        info_.orientation_ = O_ISOMETRIC;
    else if (orientation == "staggered")
        info_.orientation_ = O_STAGGERED;
    else if (orientation == "hexagonal")
        info_.orientation_ = O_HEXAGONAL;
    else
    {
        URHO3D_LOGERROR("Unsupported orientation type " + orientation);
        return false;
    }

    info_.width_ = rootElem.GetInt("width");
    info_.height_ = rootElem.GetInt("height");
    info_.tileWidth_ = rootElem.GetFloat("tilewidth") * PIXEL_SIZE;
    info_.tileHeight_ = rootElem.GetFloat("tileheight") * PIXEL_SIZE;

    for (unsigned i = 0; i < layers_.size(); ++i)
        delete layers_[i];
    layers_.clear();

    for (XMLElement childElement = rootElem.GetChild(); childElement; childElement = childElement.GetNext())
    {
        bool ret = true;
        QString name = childElement.GetName();
        if (name == "tileset")
            ret = LoadTileSet(childElement);
        else if (name == "layer")
        {
            TmxTileLayer2D* tileLayer = new TmxTileLayer2D(this);
            ret = tileLayer->Load(childElement, info_);

            layers_.push_back(tileLayer);
        }
        else if (name == "objectgroup")
        {
            TmxObjectGroup2D* objectGroup = new TmxObjectGroup2D(this);
            ret = objectGroup->Load(childElement, info_);

            layers_.push_back(objectGroup);

        }
        else if (name == "imagelayer")
        {
            TmxImageLayer2D* imageLayer = new TmxImageLayer2D(this);
            ret = imageLayer->Load(childElement, info_);

            layers_.push_back(imageLayer);
        }

        if (!ret)
        {
            loadXMLFile_.Reset();
            tsxXMLFiles_.clear();
            return false;
        }
    }

    loadXMLFile_.Reset();
    tsxXMLFiles_.clear();
    return true;
}

bool TmxFile2D::SetInfo(Orientation2D orientation, int width, int height, float tileWidth, float tileHeight)
{
    if(layers_.size()>0)
        return false;
    info_.orientation_ = orientation;
    info_.width_ = width;
    info_.height_ = height;
    info_.tileWidth_ = tileWidth * PIXEL_SIZE;
    info_.tileHeight_ = tileHeight * PIXEL_SIZE;
    return true;
}
void TmxFile2D::AddLayer(unsigned index, TmxLayer2D *layer)
{
    if(index > layers_.size())
    {
        layers_.push_back(layer);
    }
    else // index <= layers_.size()
    {
        layers_.insert(layers_.begin()+index, layer);
    }
}

void TmxFile2D::AddLayer(TmxLayer2D *layer)
{
    layers_.push_back(layer);
}

Sprite2D* TmxFile2D::GetTileSprite(int gid) const
{
    auto i = gidToSpriteMapping_.find(gid);
    if (i == gidToSpriteMapping_.end())
        return 0;

    return MAP_VALUE(i);
}
std::vector<SharedPtr<TileMapObject2D> > TmxFile2D::GetTileCollisionShapes(int gid) const
{
    std::vector<SharedPtr<TileMapObject2D> > tileShapes;
    auto i = gidToCollisionShapeMapping_.find(gid);
    if (i == gidToCollisionShapeMapping_.end())
        return tileShapes;

    return MAP_VALUE(i);
}
PropertySet2D* TmxFile2D::GetTilePropertySet(int gid) const
{
    auto i = gidToPropertySetMapping_.find(gid);
    if (i == gidToPropertySetMapping_.end())
        return 0;
    return MAP_VALUE(i);
}
const TmxLayer2D* TmxFile2D::GetLayer(unsigned index) const
{
    if (index >= layers_.size())
        return nullptr;

    return layers_[index];
}


SharedPtr<XMLFile> TmxFile2D::LoadTSXFile(const QString& source)
{
    QString tsxFilePath = GetParentPath(GetName()) + source;
    SharedPtr<File> tsxFile = context_->m_ResourceCache->GetFile(tsxFilePath);
    SharedPtr<XMLFile> tsxXMLFile(new XMLFile(context_));
    if (!tsxFile || !tsxXMLFile->Load(*tsxFile))
    {
        URHO3D_LOGERROR("Load TSX file failed " + tsxFilePath);
        return SharedPtr<XMLFile>();
    }

    return tsxXMLFile;
}

struct TileImageInfo {
    Image* image;
    int tileGid;
    int imageWidth;
    int imageHeight;
    int x;
    int y;
};
bool TmxFile2D::LoadTileSet(const XMLElement& element)
{
    int firstgid = element.GetInt("firstgid");

    XMLElement tileSetElem;
    if (element.HasAttribute("source"))
    {
        QString source = element.GetAttribute("source");
        HashMap<QString, SharedPtr<XMLFile> >::iterator i = tsxXMLFiles_.find(source);
        if (i == tsxXMLFiles_.end())
        {
            SharedPtr<XMLFile> tsxXMLFile = LoadTSXFile(source);
            if (!tsxXMLFile)
                return false;

            // Add to mapping to avoid release
            tsxXMLFiles_[source] = tsxXMLFile;

            tileSetElem = tsxXMLFile->GetRoot("tileset");
        }
        else
            tileSetElem = MAP_VALUE(i)->GetRoot("tileset");
    }
    else
        tileSetElem = element;

    int tileWidth = tileSetElem.GetInt("tilewidth");
    int tileHeight = tileSetElem.GetInt("tileheight");
    int spacing = tileSetElem.GetInt("spacing");
    int margin = tileSetElem.GetInt("margin");
    int imageWidth;
    int imageHeight;
    bool isSingleTileSet = false;

    ResourceCache* cache = context_->m_ResourceCache.get();
    {
        XMLElement imageElem = tileSetElem.GetChild("image");
        // Tileset based on single tileset image
        if (imageElem.NotNull()) {
            isSingleTileSet = true;
            QString textureFilePath = GetParentPath(GetName()) + imageElem.GetAttribute("source");
            SharedPtr<Texture2D> texture(cache->GetResource<Texture2D>(textureFilePath));
            if (!texture)
            {
                URHO3D_LOGERROR("Could not load texture " + textureFilePath);
                return false;
            }


            // Set hot spot at left bottom
            Vector2 hotSpot(0.0f, 0.0f);
            if (tileSetElem.HasChild("tileoffset"))
            {
                XMLElement offsetElem = tileSetElem.GetChild("tileoffset");
                hotSpot.x_ += offsetElem.GetFloat("x") / (float)tileWidth;
                hotSpot.y_ += offsetElem.GetFloat("y") / (float)tileHeight;
            }
            imageWidth = imageElem.GetInt("width");
            imageHeight = imageElem.GetInt("height");

            int gid = firstgid;
            for (int y = margin; y + tileHeight <= imageHeight - margin; y += tileHeight + spacing)
            {
                for (int x = margin; x + tileWidth <= imageWidth - margin; x += tileWidth + spacing)
                {
                    SharedPtr<Sprite2D> sprite(new Sprite2D(context_));
                    sprite->SetTexture(texture);
                    sprite->SetRectangle(IntRect(x, y, x + tileWidth, y + tileHeight));
                    sprite->SetHotSpot(hotSpot);

                    gidToSpriteMapping_[gid++] = sprite;
                }
            }
        }
    }

    std::vector<TileImageInfo> tileImageInfos;
    for (XMLElement tileElem = tileSetElem.GetChild("tile"); tileElem; tileElem = tileElem.GetNext("tile"))
    {
        int gid = firstgid + tileElem.GetInt("id");
        // Tileset based on collection of images
        if (!isSingleTileSet)
        {
            XMLElement imageElem = tileElem.GetChild("image");
            if (imageElem.NotNull()) {
                QString textureFilePath = GetParentPath(GetName()) + imageElem.GetAttribute("source");
                SharedPtr<Image> image(cache->GetResource<Image>(textureFilePath));
                if (!image)
                {
                    URHO3D_LOGERROR("Could not load image " + textureFilePath);
                    return false;
                }
                tileWidth = imageWidth = imageElem.GetInt("width");
                tileHeight = imageHeight = imageElem.GetInt("height");
                TileImageInfo info = {image, gid, imageWidth, imageHeight, 0, 0};
                tileImageInfos.push_back(info);
            }
        }
        // Tile collision shape(s)
        TmxObjectGroup2D objectGroup(this);
        for (XMLElement collisionElem = tileElem.GetChild("objectgroup"); collisionElem; collisionElem = collisionElem.GetNext("objectgroup"))
        {
            std::vector<SharedPtr<TileMapObject2D> > objects;
            for (XMLElement objectElem = collisionElem.GetChild("object"); objectElem; objectElem = objectElem.GetNext("object"))
            {
                SharedPtr<TileMapObject2D> object(new TileMapObject2D());

                // Convert Tiled local position (left top) to Urho3D local position (left bottom)
                objectElem.SetAttribute("y", QString::number(info_.GetMapHeight() / PIXEL_SIZE - (tileHeight - objectElem.GetFloat("y"))));

                objectGroup.StoreObject(objectElem, object, info_, true);
                objects.push_back(object);
            }
            gidToCollisionShapeMapping_[gid] = objects;
        }
        if (tileElem.HasChild("properties"))
        {
            SharedPtr<PropertySet2D> propertySet(new PropertySet2D());
            propertySet->Load(tileElem.GetChild("properties"));
            gidToPropertySetMapping_[gid] = propertySet;
        }
    }

    if (!isSingleTileSet)
    {
        if (tileImageInfos.empty())
            return false;

        AreaAllocator allocator(128, 128, 2048, 2048);

        for (int i = 0; i < tileImageInfos.size(); ++i)
        {
            TileImageInfo& info = tileImageInfos[i];
            if (!allocator.Allocate(info.imageWidth + 1, info.imageHeight + 1, info.x, info.y))
            {
                URHO3D_LOGERROR("Could not allocate area");
                return false;
            }
        }

        SharedPtr<Texture2D> texture(new Texture2D(context_));
        texture->SetMipsToSkip(QUALITY_LOW, 0);
        texture->SetNumLevels(1);
        texture->SetSize(allocator.GetWidth(), allocator.GetHeight(), Graphics::GetRGBAFormat());

        unsigned textureDataSize = allocator.GetWidth() * allocator.GetHeight() * 4;
        SharedArrayPtr<unsigned char> textureData(new unsigned char[textureDataSize]);
        memset(textureData.Get(), 0, textureDataSize);

        for (int i = 0; i < tileImageInfos.size(); ++i)
        {
            TileImageInfo& info = tileImageInfos[i];
            Image* image = info.image;

            for (int y = 0; y < image->GetHeight(); ++y)
            {
                memcpy(textureData.Get() + ((info.y + y) * allocator.GetWidth() + info.x) * 4,
                       image->GetData() + y * image->GetWidth() * 4, image->GetWidth() * 4);
            }

            SharedPtr<Sprite2D> sprite(new Sprite2D(context_));
            sprite->SetTexture(texture);
            sprite->SetRectangle(IntRect(info.x, info.y, info.x + info.imageWidth, info.y +  + info.imageHeight));
            sprite->SetHotSpot(Vector2::ZERO);
            gidToSpriteMapping_[info.tileGid] = sprite;
        }
        texture->SetData(0, 0, 0, allocator.GetWidth(), allocator.GetHeight(), textureData.Get());
    }

    return true;
}

}