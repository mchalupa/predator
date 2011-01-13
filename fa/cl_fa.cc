/*
 * Copyright (C) 2010 Jiri Simacek
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <vector>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <ctime>

#include <cl/easy.hh>
#include <cl/cl_msg.hh>
#include <cl/location.hh>
#include <cl/storage.hh>
#include <cl/cldebug.hh>
#include <cl/clutil.hh>

#include <boost/unordered_map.hpp>

#include "symctx.hh"
#include "symexec.hh"

// required by the gcc plug-in API
extern "C" { int plugin_is_GPL_compatible; }

std::ostream& operator<<(std::ostream& os, const cl_location& loc);

struct Config {

	std::string dbRoot;

	void processArg(const std::string& key, const std::string& value) {
		if (key == "db-root")
			this->dbRoot = value;
	}

	Config(const std::string& c) {
		std::vector<std::string> args;
		boost::split(args, c, boost::is_from_range(';', ';'));
		for (std::vector<std::string>::iterator i = args.begin(); i != args.end(); ++i) {
			std::vector<std::string> data;
			boost::split(data, *i, boost::is_from_range(':', ':'));
			if (data.size() == 2)
				this->processArg(data[0], data[1]);
		}			
	}

};

struct BoxDb {

	boost::unordered_map<std::string, std::string> store;

	BoxDb(const std::string& root, const std::string& fileName) {
		std::ifstream input((root + "/" + fileName).c_str());
		std::string buf;
		while (std::getline(input, buf)) {
			if (buf.empty())
				continue;
			if (buf[0] == '#')
				continue;
			std::vector<std::string> data;
			boost::split(data, buf, boost::is_from_range(':', ':'));
			if (data.size() == 2)
				this->store[data[0]] = root + "/" + data[1];
		}
	}

};

void clEasyRun(const CodeStorage::Storage& stor, const char* configString) {

    using namespace CodeStorage;

	CL_CDEBUG("config: " << configString);

	Config c(configString);

	BoxDb db(c.dbRoot, "index");

    // look for main() by name
    CL_CDEBUG("looking for 'main()' at gl scope...");
    const NameDb::TNameMap &glNames = stor.fncNames.glNames;
    const NameDb::TNameMap::const_iterator iter = glNames.find("main");
    if (glNames.end() == iter) {
        CL_WARN("main() not found at global scope");
        return;
    }

	// initialize context
	SymCtx::initCtx(stor);

    // look for definition of main()
    const FncDb &fncs = stor.fncs;
    const Fnc *main = fncs[iter->second];
    if (!main || !isDefined(*main)) {
        CL_WARN("main() not defined");
        return;
    }

	timespec start_tp, end_tp;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start_tp);

    CL_CDEBUG("starting verification stuff ...");
    try {
		SymExec se(stor);
		se.loadBoxes(db.store);
		se.run(*main);
		CL_NOTE("the program is safe ...");
	} catch (const ProgramError& e) {
		if (e.location())
			CL_ERROR_MSG(*e.location(), e.what());
		else
			CL_ERROR(e.what());
	} catch (const std::exception& e) {
		CL_ERROR(e.what());
	}

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end_tp);
	CL_NOTE("analysis took " << (end_tp.tv_sec - start_tp.tv_sec) + 1e-9*(end_tp.tv_nsec - start_tp.tv_nsec) << "s of processor time");

}
