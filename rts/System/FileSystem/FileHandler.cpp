/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "FileHandler.h"

#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <limits.h>
#include <boost/regex.hpp>

#include "lib/gml/gmlmut.h"
#include "FileQueryFlags.h"
#include "FileSystem.h"

#ifndef TOOLS
	#include "VFSHandler.h"
	#include "DataDirsAccess.h"
	#include "System/Util.h"
	#include "System/Platform/Misc.h"
#endif

using std::string;



/******************************************************************************/
/******************************************************************************/

CFileHandler::CFileHandler(const char* fileName, const char* modes)
	: ifs(NULL), filePos(0), fileSize(-1)
{
	GML_RECMUTEX_LOCK(file); // CFileHandler

	TryReadContent(fileName, modes);
}


CFileHandler::CFileHandler(const string& fileName, const string& modes)
	: ifs(NULL), filePos(0), fileSize(-1)
{
	GML_RECMUTEX_LOCK(file); // CFileHandler

	TryReadContent(fileName, modes);
}


CFileHandler::~CFileHandler()
{
	GML_RECMUTEX_LOCK(file); // ~CFileHandler

	delete ifs;
}


/******************************************************************************/

bool CFileHandler::TryReadFromPWD(const string& fileName)
{
#ifndef TOOLS
	const std::string fullpath(Platform::GetOrigCWD() + fileName);
#else
	const std::string fullpath(fileName);
#endif
	ifs = new std::ifstream(fullpath.c_str(), std::ios::in | std::ios::binary);
	if (ifs && !ifs->bad() && ifs->is_open()) {
		ifs->seekg(0, std::ios_base::end);
		fileSize = ifs->tellg();
		ifs->seekg(0, std::ios_base::beg);
		return true;
	}
	delete ifs;
	ifs = NULL;
	return false;
}


bool CFileHandler::TryReadFromRawFS(const string& fileName)
{
#ifndef TOOLS
	const string rawpath = dataDirsAccess.LocateFile(fileName);
	ifs = new std::ifstream(rawpath.c_str(), std::ios::in | std::ios::binary);
	if (ifs && !ifs->bad() && ifs->is_open()) {
		ifs->seekg(0, std::ios_base::end);
		fileSize = ifs->tellg();
		ifs->seekg(0, std::ios_base::beg);
		return true;
	}
#endif
	delete ifs;
	ifs = NULL;
	return false;
}


bool CFileHandler::TryReadFromModFS(const string& fileName)
{
#ifndef TOOLS
	if (vfsHandler == NULL) {
		return false;
	}

	const string file = StringToLower(fileName);
	if (vfsHandler->LoadFile(file, fileBuffer)) {
		// did we allocated more mem than needed
		// (e.g. because of incorrect usage of std::vector)?
		assert(fileBuffer.size() == fileBuffer.capacity()); 

		fileSize = fileBuffer.size();
		return true;
	}
#endif
	return false;
}


bool CFileHandler::TryReadFromMapFS(const string& fileName)
{
	return TryReadFromModFS(fileName); // FIXME
}


bool CFileHandler::TryReadFromBaseFS(const string& fileName)
{
	return TryReadFromModFS(fileName); // FIXME
}


void CFileHandler::TryReadContent(const string& fileName, const string& modes)
{
	this->fileName = fileName;
	const char* c = modes.c_str();
	while (c[0] != 0) {
		if ((c[0] == SPRING_VFS_RAW[0])  && TryReadFromRawFS(fileName))  break;
		if ((c[0] == SPRING_VFS_MOD[0])  && TryReadFromModFS(fileName))  break;
		if ((c[0] == SPRING_VFS_MAP[0])  && TryReadFromMapFS(fileName))  break;
		if ((c[0] == SPRING_VFS_BASE[0]) && TryReadFromBaseFS(fileName)) break;
		if ((c[0] == SPRING_VFS_PWD[0])  && TryReadFromPWD(fileName)) break;
		c++;
	}
}


/******************************************************************************/

bool CFileHandler::FileExists(const std::string& filePath, const std::string& modes)
{
	bool fileExists = false;

#ifndef TOOLS
	// VFS
	if (modes.find_first_of(SPRING_VFS_ZIP) != string::npos) {
		fileExists = fileExists || vfsHandler->FileExists(filePath);
	}

	// Raw
	if (modes.find(SPRING_VFS_RAW) != string::npos) {
		const std::string rawpath = dataDirsAccess.LocateFile(filePath);
		fileExists = fileExists || FileSystem::FileExists(rawpath);
	}
#endif

	// PWD
	if (modes.find(SPRING_VFS_PWD) != string::npos) {
	#ifndef TOOLS
		const std::string fullpath(Platform::GetOrigCWD() + filePath);
	#else
		const std::string fullpath(filePath);
	#endif
		fileExists = fileExists || FileSystem::FileExists(fullpath);
	}

	return fileExists;
}


bool CFileHandler::FileExists() const
{
	return (fileSize >= 0);
}


int CFileHandler::Read(void* buf, int length)
{
	GML_RECMUTEX_LOCK(file); // Read

	if (ifs) {
		ifs->read((char*)buf, length);
		return ifs->gcount ();
	}
	else if (!fileBuffer.empty()) {
		if ((length + filePos) > fileSize) {
			length = fileSize - filePos;
		}
		if (length > 0) {
			assert(fileBuffer.size() >= (filePos + length));
			memcpy(buf, &fileBuffer[filePos], length);
			filePos += length;
		}
		return length;
	}

	return 0;
}


void CFileHandler::Seek(int length, std::ios_base::seekdir where)
{
	GML_RECMUTEX_LOCK(file); // Seek

	if (ifs)
	{
		// on some machines, the EOF bit does not get reset when seeking to
		// another pos
		ifs->clear();
		ifs->seekg(length, where);
	}
	else if (!fileBuffer.empty())
	{
		if (where == std::ios_base::beg)
		{
			filePos = length;
		}
		else if (where == std::ios_base::cur)
		{
			filePos += length;
		}
		else if (where == std::ios_base::end)
		{
			filePos = fileSize + length;
		}
	}
}

bool CFileHandler::Eof() const
{
	GML_RECMUTEX_LOCK(file); // Eof

	if (ifs) {
		return ifs->eof();
	}
	if (!fileBuffer.empty()) {
		return (filePos >= fileSize);
	}
	return true;
}


int CFileHandler::FileSize() const
{
	return fileSize;
}


int CFileHandler::GetPos() const
{
	GML_RECMUTEX_LOCK(file); // GetPos

	if (ifs) {
		return ifs->tellg();
	} else {
		return filePos;
	}
}


bool CFileHandler::LoadStringData(string& data)
{
	GML_RECMUTEX_LOCK(file); // LoadStringData

	if (!FileExists()) {
		return false;
	}
	char* buf = new char[fileSize];
	Read(buf, fileSize);
	data.append(buf, fileSize);
	delete[] buf;
	return true;
}

std::string CFileHandler::GetFileExt() const
{
	return FileSystem::GetExtension(fileName);
}

/******************************************************************************/

std::vector<string> CFileHandler::FindFiles(const string& path,
		const string& pattern)
{
	GML_RECMUTEX_LOCK(file); // FindFiles

#ifndef TOOLS
	std::vector<string> found = dataDirsAccess.FindFiles(path, pattern);
	boost::regex regexpattern(FileSystem::ConvertGlobToRegex(pattern),
			boost::regex::icase);
	std::vector<string> f;

	if (vfsHandler) {
		f = vfsHandler->GetFilesInDir(path);
	}

	for (std::vector<string>::iterator fi = f.begin(); fi != f.end(); ++fi) {
		if (boost::regex_match(*fi, regexpattern)) {
			found.push_back(path + *fi);
		}
	}
	return found;
#else
	return std::vector<std::string>();
#endif
}


/******************************************************************************/

std::vector<string> CFileHandler::DirList(const string& path,
		const string& pattern, const string& modes)
{
	GML_RECMUTEX_LOCK(file); // DirList

	const string pat = pattern.empty() ? "*" : pattern;

	std::set<string> fileSet;
	const char* c = modes.c_str();
	while (c[0] != 0) {
		if (c[0] == SPRING_VFS_RAW[0])  { InsertRawFiles(fileSet, path, pat);  }
		if (c[0] == SPRING_VFS_MOD[0])  { InsertModFiles(fileSet, path, pat);  }
		if (c[0] == SPRING_VFS_MAP[0])  { InsertMapFiles(fileSet, path, pat);  }
		if (c[0] == SPRING_VFS_BASE[0]) { InsertBaseFiles(fileSet, path, pat); }
		c++;
	}
	std::vector<string> fileVec;
	std::set<string>::const_iterator it;
	for (it = fileSet.begin(); it != fileSet.end(); ++it) {
		fileVec.push_back(*it);
	}
	return fileVec;
}


bool CFileHandler::InsertRawFiles(std::set<string>& fileSet,
		const string& path, const string& pattern)
{
#ifndef TOOLS
	boost::regex regexpattern(FileSystem::ConvertGlobToRegex(pattern),
	                          boost::regex::icase);

	const std::vector<string> &found = dataDirsAccess.FindFiles(path, pattern);
	std::vector<string>::const_iterator fi;
	for (fi = found.begin(); fi != found.end(); ++fi) {
		if (boost::regex_match(*fi, regexpattern)) {
			fileSet.insert(fi->c_str());
		}
	}
#endif
	return true;
}


bool CFileHandler::InsertModFiles(std::set<string>& fileSet,
		const string& path, const string& pattern)
{
#ifndef TOOLS
	if (!vfsHandler) {
		return false;
	}

	string prefix = path;
	if (path.find_last_of("\\/") != (path.size() - 1)) {
		prefix += '/';
	}

	boost::regex regexpattern(FileSystem::ConvertGlobToRegex(pattern),
			boost::regex::icase);

	const std::vector<string> &found = vfsHandler->GetFilesInDir(path);
	std::vector<string>::const_iterator fi;
	for (fi = found.begin(); fi != found.end(); ++fi) {
		if (boost::regex_match(*fi, regexpattern)) {
			fileSet.insert(prefix + *fi);
		}
	}
#endif
	return true;
}


bool CFileHandler::InsertMapFiles(std::set<string>& fileSet,
		const string& path, const string& pattern)
{
	return InsertModFiles(fileSet, path, pattern); // FIXME
}


bool CFileHandler::InsertBaseFiles(std::set<string>& fileSet,
		const string& path, const string& pattern)
{
	return InsertModFiles(fileSet, path, pattern); // FIXME
}


/******************************************************************************/

std::vector<string> CFileHandler::SubDirs(const string& path,
		const string& pattern, const string& modes)
{
	GML_RECMUTEX_LOCK(file); // SubDirs

	const string pat = pattern.empty() ? "*" : pattern;

	std::set<string> dirSet;
	const char* c = modes.c_str();
	while (c[0] != 0) {
		if (c[0] == SPRING_VFS_RAW[0])  { InsertRawDirs(dirSet, path, pat);  }
		if (c[0] == SPRING_VFS_MOD[0])  { InsertModDirs(dirSet, path, pat);  }
		if (c[0] == SPRING_VFS_MAP[0])  { InsertMapDirs(dirSet, path, pat);  }
		if (c[0] == SPRING_VFS_BASE[0]) { InsertBaseDirs(dirSet, path, pat); }
		c++;
	}
	std::vector<string> dirVec;
	std::set<string>::const_iterator it;
	for (it = dirSet.begin(); it != dirSet.end(); ++it) {
		dirVec.push_back(*it);
	}
	return dirVec;
}


bool CFileHandler::InsertRawDirs(std::set<string>& dirSet,
		const string& path, const string& pattern)
{
#ifndef TOOLS
	boost::regex regexpattern(FileSystem::ConvertGlobToRegex(pattern),
	                          boost::regex::icase);

	const std::vector<string> &found = dataDirsAccess.FindFiles(path, pattern,
	                                            FileQueryFlags::ONLY_DIRS);
	std::vector<string>::const_iterator fi;
	for (fi = found.begin(); fi != found.end(); ++fi) {
		const string& dir = *fi;
		if (boost::regex_match(dir, regexpattern)) {
			dirSet.insert(dir);
		}
	}
#endif
	return true;
}


bool CFileHandler::InsertModDirs(std::set<string>& dirSet,
		const string& path, const string& pattern)
{
#ifndef TOOLS
	if (!vfsHandler) {
		return false;
	}

	string prefix = path;
	if (path.find_last_of("\\/") != (path.size() - 1)) {
		prefix += '/';
	}

	boost::regex regexpattern(FileSystem::ConvertGlobToRegex(pattern),
			boost::regex::icase);

	const std::vector<string> &found = vfsHandler->GetDirsInDir(path);
	std::vector<string>::const_iterator fi;
	for (fi = found.begin(); fi != found.end(); ++fi) {
		if (boost::regex_match(*fi, regexpattern)) {
			dirSet.insert(prefix + *fi);
		}
	}
#endif
	return true;
}


bool CFileHandler::InsertMapDirs(std::set<string>& dirSet,
		const string& path, const string& pattern)
{
	return InsertModDirs(dirSet, path, pattern); // FIXME
}


bool CFileHandler::InsertBaseDirs(std::set<string>& dirSet,
		const string& path, const string& pattern)
{
	return InsertModDirs(dirSet, path, pattern); // FIXME
}


/******************************************************************************/

string CFileHandler::AllowModes(const string& modes, const string& allowed)
{
	string newModes;
	for (unsigned i = 0; i < modes.size(); i++) {
		if (allowed.find(modes[i]) != string::npos) {
			newModes += modes[i];
		}
	}
	return newModes;
}


string CFileHandler::ForbidModes(const string& modes, const string& forbidden)
{
	string newModes;
	for (unsigned i = 0; i < modes.size(); i++) {
		if (forbidden.find(modes[i]) == string::npos) {
			newModes += modes[i];
		}
	}
	return newModes;
}


/******************************************************************************/
/******************************************************************************/
