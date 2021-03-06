/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "LoadTileData"
#include "SurfaceNode"
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/Terrain>
#include <osg/NodeVisitor>

#include <osg/ConcurrencyViewerMacros>

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

#define LC "[LoadTileData] "


LoadTileData::LoadTileData(TileNode* tilenode, EngineContext* context) :
_tilenode(tilenode),
_context(context),
_enableCancel(true)
{
    this->setTileKey(tilenode->getKey());
    setName(tilenode->getKey().str());
    _mapFrame.setMap(context->getMap());
    _engine = context->getEngine();
}

namespace
{
    struct MyProgress : public ProgressCallback {
        LoadTileData* _req;
        MyProgress(LoadTileData* req) : _req(req) {}
        bool isCanceled() {
           if (_canceled == false && _req->isIdle())
              _canceled = true;
           return ProgressCallback::isCanceled();
        }
    };
}


// invoke runs in the background pager thread.
void
LoadTileData::invoke()
{
    if (!_mapFrame.isValid())
        return;

    // we're in a pager thread, so must lock safe pointers
    // (don't access _context from here!)

    osg::ref_ptr<TileNode> tilenode;
    if (!_tilenode.lock(tilenode))
        return;

    osg::ref_ptr<TerrainEngineNode> engine;
    if (!_engine.lock(engine))
        return;

    // ensure the map frame is up to date:
    if (_mapFrame.needsSync())
        _mapFrame.sync();

    // Only use a progress callback is cancelation is enabled.    
    osg::ref_ptr<ProgressCallback> progress = _enableCancel ? new MyProgress(this) : 0L;

    // Assemble all the components necessary to display this tile
    _dataModel = engine->createTileModel(
        _mapFrame,
        tilenode->getKey(),           
        _filter,
        progress.get() );

    // if the operation was canceled, set the request to idle and delete any existing data.
    if (progress && (progress->isCanceled() || progress->needsRetry()))
    {
       _dataModel = 0L;
       setState(Request::IDLE);
    }
}


// apply() runs in the update traversal and can safely alter the scene graph
void
LoadTileData::apply(const osg::FrameStamp* stamp)
{
   osg::CVMarkerSeries series("Main Thread");
   osg::CVSpan UpdateTick(series, 4, "LoadTileData::apply");

    // ensure we got an actual datamodel:
    if (_dataModel.valid())
    {
        // ensure it's in sync with the map revision (not out of date):
        if (_dataModel->getRevision() == _context->getMap()->getDataModelRevision())
        {
            // ensure the tile node hasn't expired:
            osg::ref_ptr<TileNode> tilenode;
            if ( _tilenode.lock(tilenode) )
            {
                const RenderBindings& bindings = _context->getRenderBindings();

                // Merge the new data into the tile.
                tilenode->merge(_dataModel.get(), bindings);

                // Mark as complete. TODO: per-data requests will do something different.
                tilenode->setDirty( false );

#if 0 // gw - moved the notifications to TileNode.

                // Notify listeners that we've added a tile. The patch must be in world space
                // (include a transform). Only need to fire onTileAdded if there's real elevation data...right?
                if (_dataModel->elevationModel().valid())
                {
                    // Notify the terrain of the new tile. The "graph" needs to be
                    // the entire terrain graph since REX can load tiles out of order.
                    _context->getEngine()->getTerrain()->notifyTileAdded(
                        _dataModel->getKey(),
                        _context->getEngine()->getTerrain()->getGraph() );
                }
#endif

                OE_DEBUG << LC << "apply " << _dataModel->getKey().str() << "\n";
            }
            else
            {
                OE_DEBUG << LC << "LoadTileData failed; TileNode disappeared\n";
            }
        }
        else
        {
            OE_INFO << LC << "apply " << _dataModel->getKey().str() << " ignored b/c it is out of date\n";
        }

        // Delete the model immediately
        _dataModel = 0L;
    }
}

namespace
{
    // Fake attribute that compiles everything in the TerrainTileModel
    // when the ICO is active.
    struct ModelCompilingAttribute : public osg::Texture2D
    {
        osg::ref_ptr<TerrainTileModel> _dataModel;
        
        // the ICO calls apply() directly instead of compileGLObjects
        void apply(osg::State& state) const
        {
            _dataModel->compileGLObjects(state);
        }

        // no need to override release or resize since this is a temporary object
        // that exists only to service the ICO.

        META_StateAttribute(osgEarth, ModelCompilingAttribute, osg::StateAttribute::TEXTURE);
        int compare(const StateAttribute& sa) const { return 0; }
        ModelCompilingAttribute() { }
        ModelCompilingAttribute(const ModelCompilingAttribute& rhs, const osg::CopyOp& copy) { }
    };
}

osg::StateSet*
LoadTileData::createStateSet() const
{
    osg::ref_ptr<osg::StateSet> out;
    
    if (_dataModel.valid() &&
        _dataModel->getRevision() == _context->getMap()->getDataModelRevision())
    {
        // This stateset contains a "fake" attribute that the ICO will
        // try to GL-compile, thereby GL-compiling everything in the TerrainTileModel.
        out = new osg::StateSet();
        ModelCompilingAttribute* mca = new ModelCompilingAttribute();
        mca->_dataModel = _dataModel.get();
        out->setTextureAttribute(0, mca, 1);
    }

    return out.release();
}