#include "flat-database.h"
#include <boost/filesystem.hpp>

/** 
*   Generic Dumping and Loading Functions
*   --------------------------------------
*
*/

template<typename T>
bool LoadFlatDB(T& objToLoad, CFlatDB<T>& flatdb)
{
    CFlatDB_ReadResult readResult = flatdb.Read(objToLoad);
    if (readResult == FileError)
        LogPrintf("Missing masternode cache file - %s, will try to recreate\n", objToLoad.GetFilename());
    else if (readResult != Ok)
    {
        LogPrintf("Error reading %s: ", objToLoad.GetFilename());
        if(readResult == IncorrectFormat)
        {
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        }
        else {
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
            // program should exit with an error
            return false;
        }
    }

    LogPrintf("Reading info from %s...\n", objToLoad.GetFilename());
    flatdb.Read(objToLoad);

    return true;
}

template<typename T>
bool DumpFlatDB(T& objToSave, CFlatDB<T>& flatdb)
{
    int64_t nStart = GetTimeMillis();

    LogPrintf("Verifying %s format...\n", objToSave.GetFilename());
    CFlatDB_ReadResult readResult = flatdb.Read(objToSave, true);

    // there was an error and it was not an error on file opening => do not proceed
    
    if (readResult == FileError)
        LogPrintf("Missing file - %s, will try to recreate\n", objToSave.GetFilename());
    else if (readResult != Ok)
    {
        LogPrintf("Error reading %s: ", objToSave.GetFilename());
        if(readResult == IncorrectFormat)
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        else
        {
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
            return false;
        }
    }

    LogPrintf("Writting info to %s...\n", objToSave.GetFilename());
    flatdb.Write(objToSave);
    LogPrintf("%s dump finished  %dms\n", objToSave.GetFilename(), GetTimeMillis() - nStart);

    return true;
}

/** 
*   CFlatDB 
*   -------------------------------------
*
*/

template <typename T>
CFlatDB<T>::CFlatDB(std::string strFilenameIn, std::string strMagicMessageIn)
{
    pathDB = GetDataDir() / strFilenameIn;
    strFilename = strFilenameIn;
    strMagicMessage = strMagicMessageIn;
}

template <typename T>
bool CFlatDB<T>::Write(const T& objToSave)
{
    LOCK(objToSave.cs);

    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage; // specific magic message for this type of object
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE *file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    }
    catch (std::exception &e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrintf("Written info to %s  %dms\n", strFilename, GetTimeMillis() - nStart);
    LogPrintf("     %s\n", objToSave.ToString());

    return true;
}

template <typename T>
CFlatDB_ReadResult CFlatDB<T>::Read(T& objToLoad, bool fDryRun)
{
    //LOCK(objToLoad.cs);

    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
    {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (std::exception &e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp)
    {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }


    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp)
        {
            error("%s : Invalid magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
        {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into T object
        ssObj >> objToLoad;
    }
    catch (std::exception &e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrintf("Loaded info from %s  %dms\n", strFilename, GetTimeMillis() - nStart);
    LogPrintf("     %s\n", objToLoad.ToString());
    if(!fDryRun) {
        LogPrintf("CFlatDB - cleaning....\n");
        objToLoad.CheckAndRemove();
        LogPrintf("CFlatDB - %s\n", objToLoad.ToString());
    }

    return Ok;
}