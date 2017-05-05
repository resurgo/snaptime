#include <algorithm>
#include <boost/tokenizer.hpp>

//Create primary directory structure including indiv folder. return full directory path
TStr TSTimeParser::CreatePrimDirs(TTIdVec & IdVec) {
    //get the directory names based on hash
    TStrV dirNames;
    GetPrimDirNames(IdVec, dirNames);

    TStr dir = Directory;
    for (int i=0; i<dirNames.Len(); i++) {
        dir += TStr("/") + dirNames[i];
        if (!TDir::Exists(dir)) {
            AssertR(TDir::GenDir(dir), "Could not create directory");
        }
    }
    return dir;
}

// return a vector of the primary directory names for this idvec (including indiv folder)
void TSTimeParser::GetPrimDirNames(TTIdVec & IdVec, TStrV& result) {
    int primHash = IdVec.GetPrimHashCd();
    for (int i=0; i<ModHierarchy.Len(); i++) {
        int rem = primHash % ModHierarchy[i];
        result.Add(TInt(rem).GetStr());
    }
    result.Add(TSTimeParser::CreateIDVFileName(IdVec));
}

TVec<TStr> TSTimeParser::readCSVLine(std::string line, char delim) {
    // escape \, fields separated by ",", fields can be quoted with "
    boost::escaped_list_separator<char> sep( '\\', delim, '"' ) ;
    typedef boost::tokenizer< boost::escaped_list_separator<char> > boost_tokenizer;
    boost_tokenizer tok( line, sep );
    TVec<TStr> vec_line;
    for(boost_tokenizer::iterator beg= tok.begin(); beg!=tok.end(); ++beg)
    {
        vec_line.Add(TStr((*beg).c_str()));
    }
    return vec_line;
}

/// Should be ID, ID, ID... timestamp, value
void TSTimeParser::ReadEventFile(std::string filename) {
    std::ifstream infile(filename);
    // AssertR(inFile.is_open(), TStr("could not open file " + filename));
    std::string line;
    std::cout << "Num Records " << CurrNumRecords << std::endl;
    while(std::getline(infile, line)) {
        TTIdVec IDVector = TSTimeParser::readCSVLine(line);
        if (CurrNumRecords % 1000 == 0) std::cout << "lines read " << CurrNumRecords << std::endl;
        AssertR(IDVector.Len() >= 2, "must have at least a TS and value");

        TStr value = IDVector.Last();
        IDVector.DelLast();

        TTime ts = IDVector.Last().GetUInt64();
        IDVector.DelLast();

        TTRawData new_tv_pair = TTRawData(ts, value);

        if (!RawTimeData.IsKey(IDVector)) {
            TVec<TTRawData> new_time_data;
            new_time_data.Add(new_tv_pair);
            RawTimeData.AddDat(IDVector, new_time_data);
        } else {
            RawTimeData.GetDat(IDVector).Add(new_tv_pair);
        }
        CurrNumRecords++;
        if (CurrNumRecords >= MaxRecordCapacity) {
            std::cout << MaxRecordCapacity << std::endl;
            FlushUnsortedData();
            CurrNumRecords = 0;
        }
    }
    FlushUnsortedData();
}

//fname is based on primary and secondary hash of ids
// primHash_secHash (does not include .bin)
TStr TSTimeParser::CreateIDVFileName(TTIdVec & IdVec) {
    TStr prim_hash = TInt::GetHexStr(IdVec.GetPrimHashCd()); //dirnames are based on hash of ids
    TStr sec_hash = TInt::GetHexStr(IdVec.GetSecHashCd()); //dirnames are based on hash of ids
    TStr result = prim_hash + TStr("_") + sec_hash;
    return result;
}

void TSTimeParser::FlushUnsortedData() {
    std::cout<< "Flushing data"<<std::endl;
    THash<TTIdVec, TVec<TTRawData> >::TIter it;
    time_t t = std::time(0);
    TUInt64 now = static_cast<uint64> (t);

    for (it = RawTimeData.BegI(); it != RawTimeData.EndI(); it++) {
        TTIdVec IdVec = it.GetKey();
        TVec<TTRawData> dat = it.GetDat();

        TUnsortedTime time_record(IdVec, dat);
        // create primary structure as necessary
        TStr dir_prefix = CreatePrimDirs(IdVec);
        TStr fn = dir_prefix + TStr('/') + now.GetStr() + TStr(".bin");
        TFOut outstream(fn);
        time_record.Save(outstream);
    }
}

void TSTimeParser::SortBucketedData(bool ClearData) {
    std::cout << Directory.CStr() << std::endl;
    TStrV FnV;
    int hierarchySize = ModHierarchy.Len() +1 ;// including the top level directory
    TSTimeParser::TraverseAndSortData(Directory, hierarchySize, ClearData);

}

// ----------
// STATIC
// ----------

void TSTimeParser::TraverseAndSortData(TStr Dir, int level, bool ClearData) {
    AssertR(level >= 0, "invalid level");
    if (level == 0) {
        SortBucketedDataDir(Dir, INTEGER, ClearData);
        std::cout<< Dir.CStr() << std::endl;
    } else {
        TStrV FnV;
        TTimeFFile::GetAllFiles(Dir, FnV, true); // get the directories
        for (int i=0; i<FnV.Len(); i++) {
            TraverseAndSortData(FnV[i], level - 1, ClearData);
        }
    }
}

// TODO: what if the vector is too big to hold in memory
void TSTimeParser::SortBucketedDataDir(TStr DirPath, TType type, bool ClearData) {
    TStrV FnV;
    // retrieve filenames
    TFFile::GetFNmV(DirPath, TStrV::GetV("bin"), false, FnV);
    TUnsortedTime unsorted_record;
    TTRawDataV BucketedData;
    for (int i=0; i<FnV.Len(); i++) {
        TStr filename = FnV[i];
        TFIn infile(filename);
        unsorted_record.Load(infile);
        BucketedData.AddV(unsorted_record.TimeData);
    }
    TTIdVec IDs = unsorted_record.KeyIds;
    RawDataCmp comparator;
    BucketedData.SortCmp(comparator);
    switch (type) {
        case BOOLEAN:
            TSTimeParser::WriteSortedData<TBool>(DirPath, IDs, BucketedData,
                [] (TStr s) {return TBool(s[0] == 'T' || s[0] == 't' || s[0] == '1');}, ClearData);
            break;
        case STRING:
            TSTimeParser::WriteSortedData<TStr>(DirPath, IDs, BucketedData,
                [] (TStr s) { return s;}, ClearData);
            break;
        case INTEGER:
            TSTimeParser::WriteSortedData<TInt>(DirPath, IDs, BucketedData,
                [] (TStr s) { return TInt(s.GetInt());}, ClearData);
            break;
        default:
            TSTimeParser::WriteSortedData<TFlt>(DirPath, IDs, BucketedData,
                [] (TStr s) { return TFlt(s.GetFlt());}, ClearData);
            break;
    }
}


