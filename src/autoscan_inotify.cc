/*MT*
    
    MediaTomb - http://www.mediatomb.cc/
    
    autoscan_inotify.cc - this file is part of MediaTomb.
    
    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>
    
    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.
    
    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
    
    $Id$
*/

/// \file autoscan_inotify.cc

#ifdef HAVE_INOTIFY

#include <cassert>

#include "autoscan_inotify.h"
#include "content_manager.h"

#include <dirent.h>
#include <sys/stat.h>

#define AUTOSCAN_INOTIFY_INITIAL_QUEUE_SIZE 20

#define INOTIFY_MAX_USER_WATCHES_FILE "/proc/sys/fs/inotify/max_user_watches"

using namespace zmm;
using namespace std;

AutoscanInotify::AutoscanInotify(const ContentManager& cm, const Storage& sc)
    : cm(cm),
      sc(sc)
{
    if (check_path(_(INOTIFY_MAX_USER_WATCHES_FILE))) {
        try {
            int max_watches = trim_string(read_text_file(_(INOTIFY_MAX_USER_WATCHES_FILE))).toInt();
            SPDLOG_TRACE(l, "Max watches on the system: {}", max_watches);
        } catch (const Exception& ex) {
            l->error("Could not determine maximum number of inotify user watches: {}", ex.getMessage().c_str());
        }
    }

    watches = make_shared<unordered_map<int, Ref<Wd>>>();
    shutdownFlag = true;
    monitorQueue = Ref<ObjectQueue<AutoscanDirectory>>(new ObjectQueue<AutoscanDirectory>(AUTOSCAN_INOTIFY_INITIAL_QUEUE_SIZE));
    unmonitorQueue = Ref<ObjectQueue<AutoscanDirectory>>(new ObjectQueue<AutoscanDirectory>(AUTOSCAN_INOTIFY_INITIAL_QUEUE_SIZE));
    events = IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT;
}

AutoscanInotify::~AutoscanInotify()
{
    unique_lock<std::mutex> lock(mutex);
    if (!shutdownFlag) {
        SPDLOG_TRACE(l, "start");
        shutdownFlag = true;
        inotify->stop();
        lock.unlock();
        thread_.join();
        SPDLOG_TRACE(l, "inotify thread died.\n");
        inotify = nullptr;
        watches->clear();
    }
}

void AutoscanInotify::run()
{
    AutoLock lock(mutex);
    if (shutdownFlag) {
        shutdownFlag = false;
        inotify = Ref<Inotify>(new Inotify());
        thread_ = thread{ &AutoscanInotify::threadProc, this };
    }
}

void AutoscanInotify::threadProc()
{
    inotify_event* event;

    Ref<StringBuffer> pathBuf(new StringBuffer());

    while (!shutdownFlag) {
        try {
            Ref<AutoscanDirectory> adir;

            unique_lock<std::mutex> lock(mutex);
            while ((adir = unmonitorQueue->dequeue()) != nullptr) {
                lock.unlock();

                String location = normalizePathNoEx(adir->getLocation());
                if (!string_ok(location)) {
                    lock.lock();
                    continue;
                }

                if (adir->getRecursive()) {
                    SPDLOG_TRACE(l, "removing recursive watch: {}", location.c_str());
                    monitorUnmonitorRecursive(location, true, adir, location, true);
                } else {
                    SPDLOG_TRACE(l, "removing non-recursive watch: {}", location.c_str());
                    unmonitorDirectory(location, adir);
                }

                lock.lock();
            }

            while ((adir = monitorQueue->dequeue()) != nullptr) {
                lock.unlock();

                String location = normalizePathNoEx(adir->getLocation());
                if (!string_ok(location)) {
                    lock.lock();
                    continue;
                }

                if (adir->getRecursive()) {
                    SPDLOG_TRACE(l, "adding recursive watch: {}", location.c_str());
                    monitorUnmonitorRecursive(location, false, adir, location, true);
                } else {
                    SPDLOG_TRACE(l, "adding non-recursive watch: {}", location.c_str());
                    monitorDirectory(location, adir, location, true);
                }
                cm.rescanDirectory(adir->getObjectID(), adir->getScanID(), adir->getScanMode(), nullptr, false);

                lock.lock();
            }

            lock.unlock();

            /* --- get event --- (blocking) */
            event = inotify->nextEvent();
            /* --- */

            if (event) {
                int wd = event->wd;
                int mask = event->mask;
                String name = event->name;
                SPDLOG_TRACE(l, "inotify event: {} %x {}", wd, mask, name.c_str());

                Ref<Wd> wdObj = nullptr;
                try {
                    wdObj = watches->at(wd);
                } catch (const out_of_range& ex) {
                    inotify->removeWatch(wd);
                    continue;
                }

                pathBuf->clear();
                *pathBuf << wdObj->getPath();
                if (!(mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)))
                    *pathBuf << name;
                String path = pathBuf->toString();

                Ref<AutoscanDirectory> adir;
                Ref<WatchAutoscan> watchAs = getAppropriateAutoscan(wdObj, path);
                if (watchAs != nullptr)
                    adir = watchAs->getAutoscanDirectory();
                else
                    adir = nullptr;

                if (mask & IN_MOVE_SELF) {
                    checkMoveWatches(wd, wdObj);
                }

                if (mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                    recheckNonexistingMonitors(wd, wdObj);
                }

                if (mask & IN_ISDIR) {
                    if (mask & (IN_CREATE | IN_MOVED_TO)) {
                        recheckNonexistingMonitors(wd, wdObj);
                    }

                    if (adir != nullptr && adir->getRecursive()) {
                        if (mask & IN_CREATE) {
                            if (adir->getHidden() || name.charAt(0) != '.') {
                                SPDLOG_TRACE(l, "new dir detected, adding to inotify: {}", path.c_str());
                                monitorUnmonitorRecursive(path, false, adir, watchAs->getNormalizedAutoscanPath(), false);
                            } else {
                                SPDLOG_TRACE(l, "new dir detected, irgnoring because it's hidden: {}", path.c_str());
                            }
                        }
                    }
                }

                if (adir != nullptr && mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_UNMOUNT | IN_CREATE)) {
                    String fullPath;
                    if (mask & IN_ISDIR)
                        fullPath = path + DIR_SEPARATOR;
                    else
                        fullPath = path;

                    if (!(mask & (IN_MOVED_TO | IN_CREATE))) {
                        SPDLOG_TRACE(l, "deleting {}", fullPath.c_str());

                        if (mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                            if (IN_MOVE_SELF)
                                inotify->removeWatch(wd);
                            Ref<WatchAutoscan> watch = getStartPoint(wdObj);
                            if (watch != nullptr) {
                                if (adir->persistent()) {
                                    monitorNonexisting(path, watch->getAutoscanDirectory(), watch->getNormalizedAutoscanPath());
                                    cm->handlePeristentAutoscanRemove(adir->getScanID(), ScanMode::INotify);
                                }
                            }
                        }

                        int objectID = st->findObjectIDByPath(fullPath);
                        if (objectID != INVALID_OBJECT_ID)
                            cm->removeObject(objectID);
                    }
                    if (mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
                        SPDLOG_TRACE(l, "adding {}", path.c_str());
                        // path, recursive, async, hidden, low priority, cancellable
                        cm->addFile(fullPath, adir->getRecursive(), true, adir->getHidden(), true, false);

                        if (mask & IN_ISDIR)
                            monitorUnmonitorRecursive(path, false, adir, watchAs->getNormalizedAutoscanPath(), false);
                    }
                }
                if (mask & IN_IGNORED) {
                    removeWatchMoves(wd);
                    removeDescendants(wd);
                    watches->erase(wd);
                }
            }
        } catch (const Exception& e) {
            l->error("Inotify thread caught exception: {}", e.getMessage().c_str());
            e.printStackTrace();
        }
    }
}

void AutoscanInotify::monitor(zmm::Ref<AutoscanDirectory> dir)
{
    assert(dir->getScanMode() == ScanMode::INotify);
    SPDLOG_TRACE(l, "Requested to monitor \"{}\"", dir->getLocation().c_str());
    AutoLock lock(mutex);
    monitorQueue->enqueue(dir);
    inotify->stop();
}

void AutoscanInotify::unmonitor(zmm::Ref<AutoscanDirectory> dir)
{
    // must not be persistent
    assert(!dir->persistent());

    SPDLOG_TRACE(l, "Requested to stop monitoring \"{}\"", dir->getLocation().c_str());
    AutoLock lock(mutex);
    unmonitorQueue->enqueue(dir);
    inotify->stop();
}

int AutoscanInotify::watchPathForMoves(String path, int wd)
{
    Ref<Array<StringBase>> pathAr = split_string(path, DIR_SEPARATOR);
    Ref<StringBuffer> buf(new StringBuffer());
    int parentWd = INOTIFY_ROOT;
    for (int i = -1; i < pathAr->size() - 1; i++) {
        if (i != 0)
            *buf << DIR_SEPARATOR;
        if (i >= 0)
            *buf << pathAr->get(i);
        SPDLOG_TRACE(l, "adding move watch: {}", buf->c_str());
        parentWd = addMoveWatch(buf->toString(), wd, parentWd);
    }
    return parentWd;
}

int AutoscanInotify::addMoveWatch(String path, int removeWd, int parentWd)
{
    int wd = inotify->addWatch(path, events);
    if (wd >= 0) {
        bool alreadyThere = false;

        Ref<Wd> wdObj = nullptr;
        try {
            wdObj = watches->at(wd);

            int parentWdSet = wdObj->getParentWd();
            if (parentWdSet >= 0) {
                if (parentWd != parentWdSet) {
                    SPDLOG_TRACE(l, "error: parentWd doesn't match wd: {}, parent is: {}, should be: {}", wd, parentWdSet, parentWd);
                    wdObj->setParentWd(parentWd);
                }
            } else
                wdObj->setParentWd(parentWd);

            //find
            //alreadyThere =...
            //FIXME: not finished?

        } catch (const out_of_range& ex) {
            wdObj = Ref<Wd>(new Wd(path, wd, parentWd));
            watches->emplace(wd, wdObj);
        }

        if (!alreadyThere) {
            Ref<WatchMove> watch(new WatchMove(removeWd));
            wdObj->getWdWatches()->append(RefCast(watch, Watch));
        }
    }
    return wd;
}

void AutoscanInotify::monitorNonexisting(String path, Ref<AutoscanDirectory> adir, String normalizedAutoscanPath)
{
    String pathTmp = path;
    Ref<Array<StringBase>> pathAr = split_string(path, DIR_SEPARATOR);
    recheckNonexistingMonitor(-1, pathAr, adir, normalizedAutoscanPath);
}

void AutoscanInotify::recheckNonexistingMonitor(int curWd, Ref<Array<StringBase>> pathAr, Ref<AutoscanDirectory> adir, String normalizedAutoscanPath)
{
    Ref<StringBuffer> buf(new StringBuffer());
    bool first = true;
    for (int i = pathAr->size(); i >= 0; i--) {
        buf->clear();
        if (i == 0)
            *buf << DIR_SEPARATOR;
        else {
            for (int j = 0; j < i; j++) {
                *buf << DIR_SEPARATOR << pathAr->get(j);
                //                SPDLOG_TRACE(l, "adding: {}\n", pathAr->get(j)->data);
            }
        }
        bool pathExists = check_path(buf->toString(), true);
        //        SPDLOG_TRACE(l, "checking {}: %d\n", buf->c_str(), pathExists);
        if (pathExists) {
            if (curWd != -1)
                removeNonexistingMonitor(curWd, watches->at(curWd), pathAr);

            String path = buf->toString() + DIR_SEPARATOR;
            if (first) {
                monitorDirectory(path, adir, normalizedAutoscanPath, true);
                ContentManager::getInstance()->handlePersistentAutoscanRecreate(adir->getScanID(), adir->getScanMode());
            } else {
                monitorDirectory(path, adir, normalizedAutoscanPath, false, pathAr);
            }
            break;
        }
        if (first)
            first = false;
    }
}

void AutoscanInotify::checkMoveWatches(int wd, Ref<Wd> wdObj)
{
    Ref<Watch> watch;
    Ref<WatchMove> watchMv;
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Move) {
            if (wdWatches->size() == 1) {
                inotify->removeWatch(wd);
            } else {
                wdWatches->removeUnordered(i);
            }

            watchMv = RefCast(watch, WatchMove);
            int removeWd = watchMv->getRemoveWd();
            try {
                Ref<Wd> wdToRemove = watches->at(removeWd);

                recheckNonexistingMonitors(removeWd, wdToRemove);

                String path = wdToRemove->getPath();
                SPDLOG_TRACE(l, "found wd to remove because of move event: {} {}", removeWd, path.c_str());

                inotify->removeWatch(removeWd);
                Ref<ContentManager> cm = ContentManager::getInstance();
                Ref<WatchAutoscan> watch = getStartPoint(wdToRemove);
                if (watch != nullptr) {
                    Ref<AutoscanDirectory> adir = watch->getAutoscanDirectory();
                    if (adir->persistent()) {
                        monitorNonexisting(path, adir, watch->getNormalizedAutoscanPath());
                        cm->handlePeristentAutoscanRemove(adir->getScanID(), ScanMode::INotify);
                    }

                    int objectID = Storage::getInstance()->findObjectIDByPath(path);
                    if (objectID != INVALID_OBJECT_ID)
                        cm->removeObject(objectID);
                }
            } catch (const out_of_range& ex) {
            } // Not found in map
        }
    }
}

void AutoscanInotify::recheckNonexistingMonitors(int wd, Ref<Wd> wdObj)
{
    Ref<Watch> watch;
    Ref<WatchAutoscan> watchAs;
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Autoscan) {
            watchAs = RefCast(watch, WatchAutoscan);
            Ref<Array<StringBase>> pathAr = watchAs->getNonexistingPathArray();
            if (pathAr != nullptr) {
                recheckNonexistingMonitor(wd, pathAr, watchAs->getAutoscanDirectory(), watchAs->getNormalizedAutoscanPath());
            }
        }
    }
}

void AutoscanInotify::removeNonexistingMonitor(int wd, Ref<Wd> wdObj, Ref<Array<StringBase>> pathAr)
{
    Ref<Watch> watch;
    Ref<WatchAutoscan> watchAs;
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Autoscan) {
            watchAs = RefCast(watch, WatchAutoscan);
            if (watchAs->getNonexistingPathArray() == pathAr) {
                if (wdWatches->size() == 1) {
                    // should be done automatically, because removeWatch triggers an IGNORED event
                    //watches->remove(wd);

                    inotify->removeWatch(wd);
                } else {
                    wdWatches->removeUnordered(i);
                }
                return;
            }
        }
    }
}

void AutoscanInotify::monitorUnmonitorRecursive(String startPath, bool unmonitor, Ref<AutoscanDirectory> adir, String normalizedAutoscanPath, bool startPoint)
{
    String location;
    if (unmonitor)
        unmonitorDirectory(startPath, adir);
    else {
        bool ok = (monitorDirectory(startPath, adir, normalizedAutoscanPath, startPoint) > 0);
        if (!ok)
            return;
    }

    struct dirent* dent;
    struct stat statbuf;

    DIR* dir = opendir(startPath.c_str());
    if (!dir) {
        l->warn("Could not open {}", startPath.c_str());
        return;
    }

    while ((dent = readdir(dir)) != nullptr && !shutdownFlag) {
        char* name = dent->d_name;
        if (name[0] == '.') {
            if (name[1] == 0)
                continue;
            else if (name[1] == '.' && name[2] == 0)
                continue;
        }

        String fullPath = startPath + DIR_SEPARATOR + name;

        if (stat(fullPath.c_str(), &statbuf) != 0)
            continue;

        if (S_ISDIR(statbuf.st_mode)) {
            monitorUnmonitorRecursive(fullPath, unmonitor, adir, normalizedAutoscanPath, false);
        }
    }

    closedir(dir);
}

int AutoscanInotify::monitorDirectory(String pathOri, Ref<AutoscanDirectory> adir, String normalizedAutoscanPath, bool startPoint, Ref<Array<StringBase>> pathArray)
{
    String path = pathOri;
    if (path.length() > 0 && path[path.length() - 1] != DIR_SEPARATOR) {
        path = path + DIR_SEPARATOR;
    }

    int wd = inotify->addWatch(path, events);
    if (wd < 0) {
        if (startPoint && adir->persistent()) {
            monitorNonexisting(path, adir, normalizedAutoscanPath);
        }
    } else {
        bool alreadyWatching = false;
        int parentWd = INOTIFY_UNKNOWN_PARENT_WD;
        if (startPoint)
            parentWd = watchPathForMoves(pathOri, wd);

        Ref<Wd> wdObj = nullptr;
        try {
            wdObj = watches->at(wd);
            if (parentWd >= 0 && wdObj->getParentWd() < 0) {
                wdObj->setParentWd(parentWd);
            }

            if (pathArray == nullptr)
                alreadyWatching = (getAppropriateAutoscan(wdObj, adir) != nullptr);

            // should we check for already existing "nonexisting" watches?
            // ...
        } catch (const out_of_range& ex) {
            wdObj = Ref<Wd>(new Wd(path, wd, parentWd));
            watches->emplace(wd, wdObj);
        }

        if (!alreadyWatching) {
            Ref<WatchAutoscan> watch(new WatchAutoscan(startPoint, adir, normalizedAutoscanPath));
            if (pathArray != nullptr) {
                watch->setNonexistingPathArray(pathArray);
            }
            wdObj->getWdWatches()->append(RefCast(watch, Watch));

            if (!startPoint) {
                int startPointWd = inotify->addWatch(normalizedAutoscanPath, events);
                SPDLOG_TRACE(l, "getting start point for {} -> {} wd={}", pathOri.c_str(), normalizedAutoscanPath.c_str(), startPointWd);
                if (wd >= 0)
                    addDescendant(startPointWd, wd, adir);
            }
        }
    }
    return wd;
}

void AutoscanInotify::unmonitorDirectory(String path, Ref<AutoscanDirectory> adir)
{
    if (path.length() > 0 && path[path.length() - 1] != DIR_SEPARATOR) {
        path = path + DIR_SEPARATOR;
    }

    // maybe there is a faster method...
    // we use addWatch, because it returns the wd to the filename
    // this should not add a new watch, because it should be already watched
    int wd = inotify->addWatch(path, events);

    if (wd < 0) {
        // doesn't seem to be monitored currently
        SPDLOG_TRACE(l, "unmonitorDirectory called, but it isn't monitored? ({})", path.c_str());
        return;
    }

    Ref<Wd> wdObj = watches->at(wd);
    if (wdObj == nullptr) {
        l->error("wd not found in watches!? ({}, {})", wd, path.c_str());
        return;
    }

    Ref<WatchAutoscan> watchAs = getAppropriateAutoscan(wdObj, adir);
    if (watchAs == nullptr) {
        SPDLOG_TRACE(l, "autoscan not found in watches? ({}, {})", wd, path.c_str());
    } else {
        if (wdObj->getWdWatches()->size() == 1) {
            // should be done automatically, because removeWatch triggers an IGNORED event
            //watches->remove(wd);

            inotify->removeWatch(wd);
        } else {
            removeFromWdObj(wdObj, watchAs);
        }
    }
}

Ref<AutoscanInotify::WatchAutoscan> AutoscanInotify::getAppropriateAutoscan(Ref<Wd> wdObj, Ref<AutoscanDirectory> adir)
{
    Ref<Watch> watch;
    Ref<WatchAutoscan> watchAs;
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Autoscan) {
            watchAs = RefCast(watch, WatchAutoscan);
            if (watchAs->getNonexistingPathArray() == nullptr) {
                if (watchAs->getAutoscanDirectory()->getLocation() == adir->getLocation()) {
                    return watchAs;
                }
            }
        }
    }
    return nullptr;
}

Ref<AutoscanInotify::WatchAutoscan> AutoscanInotify::getAppropriateAutoscan(Ref<Wd> wdObj, String path)
{
    String pathBestMatch;
    Ref<WatchAutoscan> bestMatch = nullptr;
    Ref<Watch> watch;
    Ref<WatchAutoscan> watchAs;
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Autoscan) {
            watchAs = RefCast(watch, WatchAutoscan);
            if (watchAs->getNonexistingPathArray() == nullptr) {
                String testLocation = watchAs->getNormalizedAutoscanPath();
                if (path.startsWith(testLocation)) {
                    if (string_ok(pathBestMatch)) {
                        if (pathBestMatch.length() < testLocation.length()) {
                            pathBestMatch = testLocation;
                            bestMatch = watchAs;
                        }
                    } else {
                        pathBestMatch = testLocation;
                        bestMatch = watchAs;
                    }
                }
            }
        }
    }
    return bestMatch;
}

void AutoscanInotify::removeWatchMoves(int wd)
{
    Ref<Wd> wdObj;
    Ref<Array<Watch>> wdWatches;
    Ref<Watch> watch;
    Ref<WatchMove> watchMv;
    bool first = true;
    int checkWd = wd;
    do {
        wdObj = nullptr;
        try {
            wdObj = watches->at(checkWd);
        } catch (const out_of_range& ex) {
            break;
        }

        wdWatches = wdObj->getWdWatches();
        if (wdWatches == nullptr)
            break;

        if (first) {
            first = false;
        } else {
            for (int i = 0; i < wdWatches->size(); i++) {
                watch = wdWatches->get(i);
                if (watch->getType() == WatchType::Move) {
                    watchMv = RefCast(watch, WatchMove);
                    if (watchMv->getRemoveWd() == wd) {
                        SPDLOG_TRACE(l, "removing watch move\n");
                        if (wdWatches->size() == 1)
                            inotify->removeWatch(checkWd);
                        else
                            wdWatches->removeUnordered(i);
                    }
                }
            }
        }
        checkWd = wdObj->getParentWd();
    } while (checkWd >= 0);
}

bool AutoscanInotify::removeFromWdObj(Ref<Wd> wdObj, Ref<Watch> toRemove)
{
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    Ref<Watch> watch;
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch == toRemove) {
            if (wdWatches->size() == 1)
                inotify->removeWatch(wdObj->getWd());
            else
                wdWatches->removeUnordered(i);
            return true;
        }
    }
    return false;
}

bool AutoscanInotify::removeFromWdObj(Ref<Wd> wdObj, Ref<WatchAutoscan> toRemove)
{
    return removeFromWdObj(wdObj, RefCast(toRemove, Watch));
}

bool AutoscanInotify::removeFromWdObj(Ref<Wd> wdObj, Ref<WatchMove> toRemove)
{
    return removeFromWdObj(wdObj, RefCast(toRemove, Watch));
}

Ref<AutoscanInotify::WatchAutoscan> AutoscanInotify::getStartPoint(Ref<Wd> wdObj)
{
    Ref<Watch> watch;
    Ref<WatchAutoscan> watchAs;
    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Autoscan) {
            watchAs = RefCast(watch, WatchAutoscan);
            if (watchAs->isStartPoint())
                return watchAs;
        }
    }
    return nullptr;
}

void AutoscanInotify::addDescendant(int startPointWd, int addWd, Ref<AutoscanDirectory> adir)
{
    //    SPDLOG_TRACE(l, "called for %d, (adir->path={}); adding %d\n", startPointWd, adir->getLocation().c_str(), addWd);
    Ref<Wd> wdObj = nullptr;
    try {
        wdObj = watches->at(startPointWd);
    } catch (const std::out_of_range& ex) {
        return;
    }
    if (wdObj == nullptr)
        return;

    //   SPDLOG_TRACE(l, "found wdObj\n");
    Ref<WatchAutoscan> watch = getAppropriateAutoscan(wdObj, adir);
    if (watch == nullptr)
        return;
    //    SPDLOG_TRACE(l, "adding descendant\n");
    watch->addDescendant(addWd);
    //    SPDLOG_TRACE(l, "added descendant to %d (adir->path={}): %d; now: {}\n", startPointWd, adir->getLocation().c_str(), addWd, watch->getDescendants()->toCSV().c_str());
}

void AutoscanInotify::removeDescendants(int wd)
{
    Ref<Wd> wdObj = nullptr;
    try {
        wdObj = watches->at(wd);
    } catch (const out_of_range& ex) {
        return;
    }

    Ref<Array<Watch>> wdWatches = wdObj->getWdWatches();
    if (wdWatches == nullptr)
        return;

    Ref<Watch> watch;
    Ref<WatchAutoscan> watchAs;
    for (int i = 0; i < wdWatches->size(); i++) {
        watch = wdWatches->get(i);
        if (watch->getType() == WatchType::Autoscan) {
            watchAs = RefCast(watch, WatchAutoscan);
            Ref<IntArray> descendants = watchAs->getDescendants();
            if (descendants != nullptr) {
                for (int i = 0; i < descendants->size(); i++) {
                    int descWd = descendants->get(i);
                    inotify->removeWatch(descWd);
                }
            }
        }
    }
}

String AutoscanInotify::normalizePathNoEx(String path)
{
    try {
        return normalizePath(path);
    } catch (const Exception& e) {
        l->error("{}", e.getMessage().c_str());
        return nullptr;
    }
}

#endif // HAVE_INOTIFY
