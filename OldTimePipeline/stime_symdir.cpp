/* for now these all get converted to floats. this should change eventually */
void TSTimeSymDir::InflateData(TQueryResult & r, TStr initialTs, int duration, int granularity,
	std::vector<std::vector<double> > & result) {

	TTime initialTimestamp = Schema.ConvertTime(initialTs);
	int indices[r.Len()];
	int size = duration/granularity;
	for (int i=0; i< r.Len(); i++) {
		std::vector<double> empty_row (size);
		result.push_back(empty_row);
		indices[0] = 0;
	}
	for (int i=0; i < size; i++) { // for each timestamp
		TTime ts = initialTimestamp + i*duration;
		for (int j=0; j<r.Len(); j++) { // for each result
 			TSTime & data = r[j];
 			int new_index;
 			switch(data.stime_type) {
 				case BOOLEAN: new_index = TSTimeSymDir::AdvanceIndex <TBool> (data, ts, indices[j]); break;
		        case STRING: AssertR(false, "cannot yet inflate strings"); return;
		        case INTEGER: new_index = TSTimeSymDir::AdvanceIndex <TInt> (data, ts, indices[j]); break;
		        case FLOAT: new_index = TSTimeSymDir::AdvanceIndex <TFlt> (data, ts, indices[j]); break;
		    }
		        	
 			indices[j] = new_index; // update running index
 			result[j][i] = TSTimeSymDir::GetValFromResult(data, new_index);
		}
	}
}

double TSTimeSymDir::GetValFromResult(TSTime & data, int index) {
	double result = 0;
	switch(data.stime_type) {
		case BOOLEAN: 
			result = (double) ((TVec<TPair<TTime, TBool> > *) data.TimeDataPtr)->GetVal(index).GetVal2();
			break;
        case STRING: 
        	AssertR(false, "cannot yet inflate strings");
        	result = -1;
        	break;
        case INTEGER:
			result =  double(((TVec<TPair<TTime, TInt> > *) data.TimeDataPtr)->GetVal(index).GetVal2());
			break;
        case FLOAT:
			result = ((TVec<TPair<TTime, TFlt> > *) data.TimeDataPtr)->GetVal(index).GetVal2();
			break;
	}
	return result;
}

// Returns a query result. If OutputFile is not "", save into OutputFile
void TSTimeSymDir::QueryFileSys(TVec<FileQuery> Query, TQueryResult & r, TStr OutputFile) {
	// First find places where we can index by the symbolic filesystem
	THash<TStr, FileQuery> QueryMap;
	GetQuerySet(Query, QueryMap);
	TVec<FileQuery> ExtraQueries;
	// one query struct per directory split
	TVec<FileQuery> SymDirQueries(QuerySplit.Len());
	// FileQuery SymDirQueries[QuerySplit.Len()];
	for (int i=0; i<QuerySplit.Len(); i++) {
		TStr dir_name = QuerySplit[i];
		if (QueryMap.IsKey(dir_name)) {
			// query includes this directory name
			SymDirQueries[i] = QueryMap.GetDat(dir_name);
			QueryMap.DelKey(dir_name); // remove this query from the map
		} else {
			// this is empty, so we set it as an empty string
			SymDirQueries[i].QueryName = dir_name;
			SymDirQueries[i].QueryVal = TStr("");
		}
	}	
	// retrieve the data and put into an executable
	UnravelQuery(SymDirQueries, 0, OutputDir, QueryMap, r);
	if (OutputDir.Len() != 0) {
		TFOut outstream(OutputFile);
		r.Save(outstream);
	}
}

void TSTimeSymDir::GatherQueryResult(TStr FileName, THash<TStr, FileQuery> & ExtraQueries, TQueryResult & r) {
	TFIn inputstream(FileName);
	TSTime t(inputstream);

	THash<TStr, FileQuery>::TIter it;
    for (it = ExtraQueries.BegI(); it != ExtraQueries.EndI(); it++) {
        TStr QueryName = it.GetKey();
        TStr QueryVal = it.GetDat().QueryVal;
        AssertR(Schema.KeyNameToIndex.IsKey(QueryName), "Invalid query");
        TInt IdIndex = Schema.KeyNameToIndex.GetDat(QueryName);
        if (t.KeyIds[IdIndex] != QueryVal) return; // does not match query
    }
	r.Add(t);
	t.TimeDataPtr = NULL; // prevents deallocating the pointer we just added into the array.
	//TODO, above is a hack. fix this
}

void TSTimeSymDir::UnravelQuery(TVec<FileQuery> & SymDirQueries, int SymDirQueryIndex,
	TStr& Dir, THash<TStr, FileQuery> & ExtraQueries, TQueryResult & r) {
	if (SymDirQueryIndex == QuerySplit.Len()) {
		// base case: done traversing the symbolic directory, so we are in a directory
		// of pure event files. gather these event files into r
		GatherQueryResult(Dir, ExtraQueries, r);
		return;
	}
	if (SymDirQueries[SymDirQueryIndex].QueryVal != TStr("")) {
		// if this directory has a query value, go to that folder
		TStr path = Dir + TStr("/") + TTimeFFile::EscapeFileName(SymDirQueries[SymDirQueryIndex].QueryVal);
		AssertR(TDir::Exists(path), "Query does not exist in symbolic dir");
		UnravelQuery(SymDirQueries, SymDirQueryIndex+1, path, ExtraQueries, r);
	} else {
		// this directory doesn't have a query value, so queue up gathering in all subfolders
		TStrV FnV;
		TTimeFFile::GetAllFiles(Dir, FnV);
		for (int i=0; i<FnV.Len(); i++) {
			UnravelQuery(SymDirQueries, SymDirQueryIndex+1, FnV[i], ExtraQueries, r);
		}
	}
}

void TSTimeSymDir::GetQuerySet(TVec<FileQuery> & Query, THash<TStr, FileQuery> & result) {
	for (int i=0; i<Query.Len(); i++) {
		result.AddDat(Query[i].QueryName, Query[i]);
	}
}

void TSTimeSymDir::CreateSymbolicDirs() {
	if (FileSysCreated) return;
	TraverseEventFiles(InputDir);
	FileSysCreated = true;
}

void TSTimeSymDir::TraverseEventFiles(TStr& Dir) {
	if(!TDir::Exists(Dir)) {
		// this is the event file
		CreateSymDirsForEventFile(Dir);
	} else {
		TStrV FnV;
		TTimeFFile::GetAllFiles(Dir, FnV); // get the directories
		for (int i=0; i<FnV.Len(); i++) {
			TraverseEventFiles(FnV[i]);
		}
	}
}

void TSTimeSymDir::CreateSymDirsForEventFile(TStr & EventFileName) {
	std::cout<< "Processing " << EventFileName.CStr() << std::endl;
	TFIn inputstream(EventFileName);
	TSTime t;
	t.LoadMetaData(inputstream);
	TStrV SymDirs;
	std::cout << QuerySplit.Len() << std::endl;
	// find the dir names
	for (int i=0; i<QuerySplit.Len(); i++) {
		TStr & Query = QuerySplit[i];
		AssertR(Schema.KeyNameToIndex.IsKey(Query), "Query to split on SymDir not found");
		TInt IDIndex = Schema.KeyNameToIndex.GetDat(Query);
		SymDirs.Add(TTimeFFile::EscapeFileName(t.KeyIds[IDIndex]));
	}
	std::cout << SymDirs.Len() << std::endl;
	// for each directory level, create the dir if it doesn't exist
	for (int i=0; i<SymDirs.Len(); i++) {
		std::cout<< SymDirs[i].CStr() << std::endl;
		std::cout << "----" << std::endl;
	}
	//AssertR(false, "added assert");
	TStr path = OutputDir;
	for (int i=0; i<SymDirs.Len(); i++) {
		path = path + TStr("/") + SymDirs[i];
		if (!TDir::Exists(path)) {
			std::cout << "creating directory " << path.CStr() << std::endl;
			AssertR(TDir::GenDir(path), "Could not create directory");
		}
	}
	// create a sym link at the end of the path for this stime
	TStr final_path = path + TStr("/") + TCSVParse::CreateIDVFileName(t.KeyIds);
	int success = symlink(EventFileName.CStr(), final_path.CStr());
	AssertR(success != -1, "Failed to create symbolic directory");
}
