//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/usd/usd/instanceCache.h"
#include "pxr/usd/usd/debugCodes.h"

#include "pxr/usd/pcp/primIndex.h"

#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tracelite/trace.h"

#include <utility>

using std::make_pair;
using std::pair;
using std::vector;

TF_DEFINE_ENV_SETTING(
    USD_ASSIGN_MASTERS_DETERMINISTICALLY, false,
    "Set to true to cause instances to be assigned to masters in a "
    "deterministic way, ensuring consistency across runs.  This incurs "
    "some additional overhead.");

Usd_InstanceCache::Usd_InstanceCache()
    : _lastMasterIndex(0)
{
}

bool
Usd_InstanceCache::RegisterInstancePrimIndex(const PcpPrimIndex& index)
{
    TfAutoMallocTag tag("InstanceCache::RegisterIndex");

    if (not TF_VERIFY(index.IsInstanceable())) {
        return false;
    }

    // Make sure we compute the key for this index before we grab
    // the mutex to minimize the time we hold the lock.
    const Usd_InstanceKey key(index);

    // Check whether a master for this prim index already exists
    // or if this prim index is already being used as the source for
    // a master.
    _InstanceKeyToMasterMap::const_iterator keyToMasterIt = 
        _instanceKeyToMasterMap.find(key);
    const bool masterAlreadyExists = 
        (keyToMasterIt != _instanceKeyToMasterMap.end());

    tbb::spin_mutex::scoped_lock lock(_mutex);

    _PrimIndexPaths& pendingIndexes = _pendingAddedPrimIndexes[key];
    pendingIndexes.push_back(index.GetPath());

    // A new master must be created for this instance if one doesn't
    // already exist and this instance is the first one registered for
    // this key.
    const bool needsNewMaster = 
        (not masterAlreadyExists and pendingIndexes.size() == 1);
    return needsNewMaster;
}

void
Usd_InstanceCache::UnregisterInstancePrimIndexesUnder(
    const SdfPath& primIndexPath)
{
    TfAutoMallocTag tag("InstanceCache::UnregisterIndex");

    for (_PrimIndexToMasterMap::const_iterator 
             it = _primIndexToMasterMap.lower_bound(primIndexPath),
             end = _primIndexToMasterMap.end();
         it != end and it->first.HasPrefix(primIndexPath); ++it) {

        const SdfPath& masterPath = it->second;
        _MasterToInstanceKeyMap::const_iterator masterToKeyIt = 
            _masterToInstanceKeyMap.find(masterPath);
        if (not TF_VERIFY(masterToKeyIt != _masterToInstanceKeyMap.end())) {
            continue;
        }

        const Usd_InstanceKey& key = masterToKeyIt->second;
        _PrimIndexPaths& pendingIndexes = _pendingRemovedPrimIndexes[key];
        pendingIndexes.push_back(it->first);
    }
}

void 
Usd_InstanceCache::ProcessChanges(Usd_InstanceChanges* changes)
{
    TRACE_FUNCTION();
    TfAutoMallocTag tag("InstanceCache::ProcessChanges");


    // Remove unregistered prim indexes from the cache.
    for (_InstanceKeyToPrimIndexesMap::value_type &v:
             _pendingRemovedPrimIndexes) {
        const Usd_InstanceKey& key = v.first;
        _PrimIndexPaths& primIndexes = v.second;

        // Ignore any unregistered prim index that was subsequently
        // re-registered.
        _InstanceKeyToPrimIndexesMap::const_iterator registeredIt = 
            _pendingAddedPrimIndexes.find(key);
        if (registeredIt != _pendingAddedPrimIndexes.end()) {
            _PrimIndexPaths registered = registeredIt->second;
            _PrimIndexPaths unregistered;
            unregistered.swap(primIndexes);

            std::sort(registered.begin(), registered.end());
            std::sort(unregistered.begin(), unregistered.end());
            std::set_difference(unregistered.begin(), unregistered.end(),
                                registered.begin(), registered.end(),
                                std::back_inserter(primIndexes));
        }

        _RemoveInstances(key, primIndexes, changes);
    }

    // Add newly-registered prim indexes to the cache.
    if (TfGetEnvSetting(USD_ASSIGN_MASTERS_DETERMINISTICALLY)) {
        // The order in which we process newly-registered prim indexes
        // determines the name of the master prims assigned to instances.
        // We need to iterate over the hash map in a fixed ordering to
        // ensure we have a consistent assignment of instances to masters.
        typedef std::map<SdfPath, Usd_InstanceKey> _PrimIndexPathToKey;
        std::map<SdfPath, Usd_InstanceKey> keysToProcess;
        for (_InstanceKeyToPrimIndexesMap::value_type& v:
                 _pendingAddedPrimIndexes) {
            const Usd_InstanceKey& key = v.first;
            const _PrimIndexPaths& primIndexes = v.second;
            if (TF_VERIFY(not primIndexes.empty())) {
                keysToProcess[*std::min_element(
                        primIndexes.begin(), primIndexes.end())] = key;
            }
        }

        for (const _PrimIndexPathToKey::value_type& v: keysToProcess) {
            const Usd_InstanceKey& key = v.second;
            _PrimIndexPaths& primIndexes = _pendingAddedPrimIndexes[key];
            _CreateOrUpdateMasterForInstances(key, &primIndexes, changes);
        }
    }
    else {
        for(_InstanceKeyToPrimIndexesMap::value_type& v:
                _pendingAddedPrimIndexes) {
            _CreateOrUpdateMasterForInstances(v.first, &v.second, changes);
        }
    }

    // Now that we've processed all additions and removals, we can find and
    // drop any masters that have no instances associated with them.
    for (const auto& v : _pendingRemovedPrimIndexes) {
        _RemoveMasterIfNoInstances(v.first, changes);
    }

    _pendingAddedPrimIndexes.clear();
    _pendingRemovedPrimIndexes.clear();
}

void
Usd_InstanceCache::_CreateOrUpdateMasterForInstances(
    const Usd_InstanceKey& key,
    _PrimIndexPaths* primIndexPaths,
    Usd_InstanceChanges* changes)
{
    pair<_InstanceKeyToMasterMap::iterator, bool> result = 
        _instanceKeyToMasterMap.insert(make_pair(key, SdfPath()));
    
    const bool createdNewMaster = result.second;
    if (createdNewMaster) {
        // If this is a new master prim, the first instanceable prim
        // index that was registered must be selected as the source
        // index because the consumer was told that index required
        // a new master via RegisterInstancePrimIndex.
        //
        // Note that this means the source prim index for a master may
        // change from run to run. This should be fine, because all
        // prim indexes with the same instancing key should have the 
        // same composed values.
        const SdfPath newMasterPath = _GetNextMasterPath(key);
        result.first->second = newMasterPath;
        _masterToInstanceKeyMap[newMasterPath] = key;

        const SdfPath sourcePrimIndexPath = primIndexPaths->front();
        _sourcePrimIndexToMasterMap[sourcePrimIndexPath] = newMasterPath;
        _masterToSourcePrimIndexMap[newMasterPath] = sourcePrimIndexPath;

        changes->newMasterPrims.push_back(newMasterPath);
        changes->newMasterPrimIndexes.push_back(sourcePrimIndexPath);

        TF_DEBUG(USD_INSTANCING).Msg(
            "Instancing: Creating master <%s> with source prim index <%s>\n", 
            newMasterPath.GetString().c_str(),
            sourcePrimIndexPath.GetString().c_str());
    }
    else {
        // Otherwise, if a master prim for this instance already exists
        // but no source prim index has been assigned, do so here. This
        // is exactly what happens in _RemoveInstances when a new source
        // is assigned to a master; however, this handles the case where
        // the last instance of a master has been removed and a new 
        // instance of the master has been added in the same round of changes.
        const SdfPath& masterPath = result.first->second;
        const bool assignNewPrimIndexForMaster = 
            (_masterToSourcePrimIndexMap.count(masterPath) == 0);
        if (assignNewPrimIndexForMaster) {
            const SdfPath sourcePrimIndexPath = primIndexPaths->front();
            _sourcePrimIndexToMasterMap[sourcePrimIndexPath] = masterPath;
            _masterToSourcePrimIndexMap[masterPath] = sourcePrimIndexPath;

            changes->changedMasterPrims.push_back(masterPath);
            changes->changedMasterPrimIndexes.push_back(sourcePrimIndexPath);

            TF_DEBUG(USD_INSTANCING).Msg(
                "Instancing: Master <%s> assigned new source prim index <%s>\n",
                masterPath.GetText(), sourcePrimIndexPath.GetText());
        }
    }

    // Assign the newly-registered prim indexes to their master.
    const SdfPath& masterPath = result.first->second;
    for (const SdfPath& primIndexPath: *primIndexPaths) {
        _primIndexToMasterMap[primIndexPath] = masterPath;
    }

    _PrimIndexPaths& primIndexesForMaster = _masterToPrimIndexesMap[masterPath];
    std::sort(primIndexPaths->begin(), primIndexPaths->end());

    if (primIndexesForMaster.empty()) {
        primIndexesForMaster.swap(*primIndexPaths);
    }
    else {
        const size_t oldNumPrimIndexes = primIndexesForMaster.size();
        primIndexesForMaster.insert(
            primIndexesForMaster.end(),
            primIndexPaths->begin(), primIndexPaths->end());

        _PrimIndexPaths::iterator newlyAddedIt = 
            primIndexesForMaster.begin() + oldNumPrimIndexes;
        std::inplace_merge(primIndexesForMaster.begin(), newlyAddedIt,
                           primIndexesForMaster.end());

        primIndexesForMaster.erase(
            std::unique(primIndexesForMaster.begin(), 
                        primIndexesForMaster.end()),
            primIndexesForMaster.end());
    }
}

void
Usd_InstanceCache::_RemoveInstances(
    const Usd_InstanceKey& instanceKey,
    const _PrimIndexPaths& primIndexPaths,
    Usd_InstanceChanges* changes)
{
    _InstanceKeyToMasterMap::iterator keyToMasterIt = 
        _instanceKeyToMasterMap.find(instanceKey);
    if (keyToMasterIt == _instanceKeyToMasterMap.end()) {
        return;
    }

    const SdfPath& masterPath = keyToMasterIt->second;
    bool masterNeedsNewPrimIndex = false;

    // Remove the prim indexes from the prim index <-> master bidirectional
    // mapping.
    _PrimIndexPaths& primIndexesForMaster = _masterToPrimIndexesMap[masterPath];
    for (const SdfPath& path: primIndexPaths) {
        _PrimIndexPaths::iterator it = std::find(
            primIndexesForMaster.begin(), primIndexesForMaster.end(), path);
        if (it != primIndexesForMaster.end()) {
            primIndexesForMaster.erase(it);
            _primIndexToMasterMap.erase(path);
        }

        if (_sourcePrimIndexToMasterMap.erase(path)) {
            TF_VERIFY(_masterToSourcePrimIndexMap.erase(masterPath));
            masterNeedsNewPrimIndex = true;
        }
    }

    // If the source prim index for this master is no longer available
    // but we have other instance prim indexes we can use instead, select
    // one of those to serve as the new source. 
    //
    // Otherwise, do nothing; we defer removal of this master until
    // the end of instance change processing (see _RemoveMasterIfNoInstances)
    // in case a new instance for this master was registered.
    if (masterNeedsNewPrimIndex and not primIndexesForMaster.empty()) {
        const SdfPath& newSourceIndexPath = primIndexesForMaster.front();

        TF_DEBUG(USD_INSTANCING).Msg(
            "Instancing: Assigning new source <%s> for <%s>\n",
            newSourceIndexPath.GetText(), masterPath.GetText());

        _sourcePrimIndexToMasterMap[newSourceIndexPath] = masterPath;
        _masterToSourcePrimIndexMap[masterPath] = newSourceIndexPath;

        changes->changedMasterPrims.push_back(masterPath);
        changes->changedMasterPrimIndexes.push_back(newSourceIndexPath);
    }
}

void 
Usd_InstanceCache::_RemoveMasterIfNoInstances(
    const Usd_InstanceKey& instanceKey,
    Usd_InstanceChanges* changes)
{
    auto keyToMasterIt = _instanceKeyToMasterMap.find(instanceKey);
    if (keyToMasterIt == _instanceKeyToMasterMap.end()) {
        return;
    }

    const SdfPath& masterPath = keyToMasterIt->second;
    auto masterToPrimIndexesIt = _masterToPrimIndexesMap.find(masterPath);
    if (not TF_VERIFY(masterToPrimIndexesIt != _masterToPrimIndexesMap.end())) {
        return;
    }

    const _PrimIndexPaths& primIndexesForMaster = masterToPrimIndexesIt->second;
    if (primIndexesForMaster.empty()) {
        // This master has no more instances associated with it, so it can
        // be released.
        TF_DEBUG(USD_INSTANCING).Msg(
            "Instancing: Removing master <%s>\n", masterPath.GetText());

        // Do this first, since masterPath will be a stale reference after
        // removing the map entries.
        changes->deadMasterPrims.push_back(masterPath);

        _masterToInstanceKeyMap.erase(keyToMasterIt->second);
        _instanceKeyToMasterMap.erase(keyToMasterIt);

        _masterToPrimIndexesMap.erase(masterToPrimIndexesIt);
    }
}

bool 
Usd_InstanceCache::IsPathMasterOrInMaster(const SdfPath& path)
{
    if (path.IsEmpty()) {
        return false;
    }
    if (!path.IsAbsolutePath()) {
        // We require an absolute path because there is no way for us
        // to walk to the root prim level from a relative path.
        TF_CODING_ERROR("IsPathMasterOrInMaster() requires an absolute path "
                        "but was given <%s>", path.GetText());
        return false;
    }

    SdfPath rootPath = path;
    while (not rootPath.IsRootPrimPath()) {
        rootPath = rootPath.GetParentPath();
    }

    return TfStringStartsWith(rootPath.GetName(), "__Master_");
}

SdfPath 
Usd_InstanceCache::_GetNextMasterPath(const Usd_InstanceKey& key)
{
    return SdfPath::AbsoluteRootPath().AppendChild(
        TfToken(TfStringPrintf("__Master_%zu", ++_lastMasterIndex)));
}

vector<SdfPath> 
Usd_InstanceCache::GetAllMasters() const
{
    vector<SdfPath> paths;
    paths.reserve(_instanceKeyToMasterMap.size());
    for (const _InstanceKeyToMasterMap::value_type& v:
             _instanceKeyToMasterMap) {
        paths.push_back(v.second);
    }
    return paths;
}

size_t 
Usd_InstanceCache::GetNumMasters() const
{
    return _masterToInstanceKeyMap.size();
}

SdfPath 
Usd_InstanceCache::GetMasterUsingPrimIndexAtPath(
    const SdfPath& primIndexPath) const
{
    _SourcePrimIndexToMasterMap::const_iterator it = 
        _sourcePrimIndexToMasterMap.find(primIndexPath);
    return (it == _sourcePrimIndexToMasterMap.end() ? SdfPath() : it->second);
}

template <class PathMap>
static 
typename PathMap::const_iterator
_FindEntryForPathOrAncestor(const PathMap& map, SdfPath path)
{
    for (; path != SdfPath::AbsoluteRootPath(); path = path.GetParentPath()) {
        typename PathMap::const_iterator it = map.upper_bound(path);
        if (it != map.begin()) {
            --it;
            if (path.HasPrefix(it->first)) {
                return it;
            }
        }
    }
    return map.end();
}

template <class PathMap>
static 
typename PathMap::const_iterator
_FindEntryForAncestor(const PathMap& map, const SdfPath& path)
{
    if (path == SdfPath::AbsoluteRootPath()) {
        return map.end();
    }

    return _FindEntryForPathOrAncestor(map, path.GetParentPath());
}

bool 
Usd_InstanceCache::IsPrimInMasterUsingPrimIndexAtPath(
    const SdfPath& primIndexPath) const
{
    return _IsPrimInMasterUsingPrimIndexAtPath(primIndexPath);
}

vector<SdfPath> 
Usd_InstanceCache::GetPrimsInMastersUsingPrimIndexAtPath(
    const SdfPath& primIndexPath) const
{
    vector<SdfPath> masterPaths;
    _IsPrimInMasterUsingPrimIndexAtPath(primIndexPath, &masterPaths);
    return masterPaths;
}

bool
Usd_InstanceCache::_IsPrimInMasterUsingPrimIndexAtPath(
    const SdfPath& primIndexPath,
    vector<SdfPath>* masterPaths) const
{
    // This function is trickier than you might expect because it has
    // to deal with nested instances. Consider this case:
    //
    // /World
    //   Set_1     [master: </__Master_1>]
    // /__Master_1 [index: </World/Set_1>]
    //   Prop_1    [master: </__Master_2>, index: </World/Set_1/Prop_1> ]
    //   Prop_2    [master: </__Master_2>, index: </World/Set_1/Prop_2> ]
    // /__Master_2 [index: </World/Set_1/Prop_1>]
    //   Scope     [index: </World/Set_1/Prop_1/Scope>]
    // 
    // Asking if the prim index /World/Set_1/Prop_1/Scope is used by
    // a master should return true, because it is used by /__Master_2/Scope.
    // But this function should return false for /World/Set_1/Prop_2/Scope.
    // The naive implementation that looks through _sourcePrimIndexToMasterMap
    // would wind up returning true for both of these.

    bool primIndexIsUsedByMaster = false;

    SdfPath curIndexPath = primIndexPath;
    while (curIndexPath != SdfPath::AbsoluteRootPath()) {
        // Find the instance prim index that is closest to the current prim
        // index path. If there isn't one, this prim index isn't a descendent
        // of an instance, which means it can't possibly be used by a master.
        _PrimIndexToMasterMap::const_iterator it = _FindEntryForPathOrAncestor(
            _primIndexToMasterMap, curIndexPath);
        if (it == _primIndexToMasterMap.end()) {
            break;
        }

        // Figure out what master is associated with the prim index
        // we found, and see if the given prim index is a descendent of its
        // source prim index. If it is, then this prim index must be used
        // by a descendent of that master.
        _MasterToSourcePrimIndexMap::const_iterator masterToSourceIt =
            _masterToSourcePrimIndexMap.find(it->second);
        if (not TF_VERIFY(
                masterToSourceIt != _masterToSourcePrimIndexMap.end())) {
            break;
        }

        const SdfPath& masterPath = masterToSourceIt->first;
        const SdfPath& sourcePrimIndexPath = masterToSourceIt->second;
        if (curIndexPath.HasPrefix(sourcePrimIndexPath)) {
            // If we don't need to collect all the master paths using this
            // prim index, we can bail out immediately.
            primIndexIsUsedByMaster = true;
            if (masterPaths) {
                masterPaths->push_back(primIndexPath.ReplacePrefix(
                    sourcePrimIndexPath, masterPath));
            }
            else {
                break;
            }
        }

        // If we found an entry for an ancestor of curIndexPath in 
        // _primIndexToMasterMap, the index must be a descendent of an
        // instanceable prim index. These indexes can only ever be used by
        // a single master prim, so we can stop here. 
        // 
        // Otherwise, this index is an instanceable prim index. In the case of 
        // nested instancing, there may be another master prim using this index,
        // so we have to keep looking.
        const bool indexIsDescendentOfInstance = (it->first != curIndexPath);
        if (indexIsDescendentOfInstance) {
            break;
        }

        curIndexPath = it->first.GetParentPath();
    }

    return primIndexIsUsedByMaster;
}

bool 
Usd_InstanceCache::IsPrimInMasterForPrimIndexAtPath(
    const SdfPath& primIndexPath) const
{
    // If any ancestor of primIndexPath is in _primIndexToMasterMap, it's
    // a descendent of an instance.
    _PrimIndexToMasterMap::const_iterator it = _FindEntryForAncestor(
        _primIndexToMasterMap, primIndexPath);
    return it != _primIndexToMasterMap.end();
}

SdfPath 
Usd_InstanceCache::GetMasterForPrimIndexAtPath(
    const SdfPath& primIndexPath) const
{
    // Search the mapping from instance prim index to master prim
    // to find the associated master.
    _PrimIndexToMasterMap::const_iterator it = 
        _primIndexToMasterMap.find(primIndexPath);
    return (it == _primIndexToMasterMap.end() ? SdfPath() : it->second);
}

SdfPath
Usd_InstanceCache::GetPrimInMasterForPrimIndexAtPath(
    const SdfPath& primIndexPath) const
{
    SdfPath primInMasterPath;

    // This function is trickier than you might expect because it has
    // to deal with nested instances. Consider this case:
    //
    // /World
    //   Set_1     [master: </__Master_1>, index: </World/Set_1>]
    //   Set_2     [master: </__Master_1>, index: </World/Set_2>]
    // /__Master_1 [index: </World/Set_1>]
    //   Prop_1    [master: </__Master_2>, index: </World/Set_1/Prop_1> ]
    //   Prop_2    [master: </__Master_2>, index: </World/Set_1/Prop_2> ]
    // /__Master_2 [index: </World/Set_1/Prop_1>]
    //   Scope     [index: </World/Set_1/Prop_1/Scope>]
    // 
    // Asking for the prim in master for the prim index 
    // /World/Set_2/Prop_1/Scope should return /__Master_2/Scope, since
    // /World/Set_2 is an instance of /__Master_1, and /__Master_1/Prop_1
    // is an instance of /__Master_2.
    //
    // The naive implementation would look through _primIndexToMasterMap
    // and do a prefix replacement, but that gives /__Master_1/Prop_1/Scope. 
    // This is because the prim index /World/Set_2/Prop_1/Scope has never been 
    // computed in this example!

    SdfPath curPrimIndexPath = primIndexPath;
    while (not curPrimIndexPath.IsEmpty()) {
        // Find the instance prim index that is closest to the current
        // prim index path. If there isn't one, this prim index isn't a 
        // descendent of an instance.
        _PrimIndexToMasterMap::const_iterator it = 
            _FindEntryForAncestor(_primIndexToMasterMap, curPrimIndexPath);
        if (it == _primIndexToMasterMap.end()) {
            break;
        }

        // Find the source prim index corresponding to this master.
        // If curPrimIndexPath is already relative to this prim index,
        // we can do a prefix replacement to determine the final master
        // prim path.
        //
        // If curPrimIndexPath is *not* relative to this prim index,
        // do a prefix replacement to make it so, then loop and try again.
        // This helps us compute the correct prim in master in the case
        // above because we know the source prim index *must* have been
        // computed -- otherwise, it wouldn't be a master's source index.
        // The next time around we'll find a match for curPrimIndexPath 
        // in _primIndexToMasterMap that gets us closer to the nested
        // instance's master (if one exists).
        _MasterToSourcePrimIndexMap::const_iterator masterToSourceIt =
            _masterToSourcePrimIndexMap.find(it->second);
        if (not TF_VERIFY(
                masterToSourceIt != _masterToSourcePrimIndexMap.end())) {
            break;
        }
        
        const SdfPath& sourcePrimIndexPath = masterToSourceIt->second;
        if (it->first == sourcePrimIndexPath) {
            primInMasterPath = 
                curPrimIndexPath.ReplacePrefix(it->first, it->second);
            break;
        }

        curPrimIndexPath = 
            curPrimIndexPath.ReplacePrefix(it->first, sourcePrimIndexPath);
    }

    return primInMasterPath;
}
