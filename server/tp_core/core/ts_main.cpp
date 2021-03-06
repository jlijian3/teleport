#include "ts_main.h"
#include "ts_session.h"
#include "ts_http_rpc.h"
#include "ts_web_rpc.h"
#include "ts_env.h"

#include <mbedtls/platform.h>
#include <mbedtls/debug.h>

bool g_exit_flag = false;

TPP_SESSION_INFO* tpp_take_session(const char* sid)
{
	TS_SESSION_INFO sinfo;
	bool ret = g_session_mgr.take_session(sid, sinfo);
	if (!ret)
		return NULL;

	TPP_SESSION_INFO* info = (TPP_SESSION_INFO*)calloc(1, sizeof(TPP_SESSION_INFO));

	info->sid = (char*)calloc(1, sinfo.sid.length() + 1);
	ex_strcpy(info->sid, sinfo.sid.length() + 1, sinfo.sid.c_str());
	info->account_name = (char*)calloc(1, sinfo.account_name.length() + 1);
	ex_strcpy(info->account_name, sinfo.account_name.length() + 1, sinfo.account_name.c_str());
	info->host_ip = (char*)calloc(1, sinfo.host_ip.length() + 1);
	ex_strcpy(info->host_ip, sinfo.host_ip.length() + 1, sinfo.host_ip.c_str());
	info->user_name = (char*)calloc(1, sinfo.user_name.length() + 1);
	ex_strcpy(info->user_name, sinfo.user_name.length() + 1, sinfo.user_name.c_str());
	info->user_auth = (char*)calloc(1, sinfo.user_auth.length() + 1);
	ex_strcpy(info->user_auth, sinfo.user_auth.length() + 1, sinfo.user_auth.c_str());
	info->user_param = (char*)calloc(1, sinfo.user_param.length() + 1);
	ex_strcpy(info->user_param, sinfo.user_param.length() + 1, sinfo.user_param.c_str());

	info->auth_id = sinfo.auth_id;
	info->host_port = sinfo.host_port;
	info->protocol = sinfo.protocol;
	info->auth_mode = sinfo.auth_mode;
	info->sys_type = sinfo.sys_type;
	info->ref_count = sinfo.ref_count;
	info->ticket_start = sinfo.ticket_start;

	return info;
}

void tpp_free_session(TPP_SESSION_INFO* info)
{
	if (NULL == info)
		return;

	free(info->sid);
	free(info->account_name);
	free(info->host_ip);
	free(info->user_name);
	free(info->user_auth);
	free(info->user_param);
	free(info);
}

bool tpp_session_begin(const TPP_SESSION_INFO* info, int* db_id)
{
	if (NULL == info || NULL == db_id)
		return false;

	TS_SESSION_INFO sinfo;
	sinfo.sid = info->sid;
	sinfo.account_name = info->account_name;
	sinfo.auth_id = info->auth_id;
	sinfo.host_ip = info->host_ip;
	sinfo.host_port = info->host_port;
	sinfo.protocol = info->protocol;
	sinfo.user_name = info->user_name;
	sinfo.user_auth = info->user_auth;
	sinfo.user_param = info->user_param;
	sinfo.auth_mode = info->auth_mode;
	sinfo.sys_type = info->sys_type;
	sinfo.ref_count = info->ref_count;
	sinfo.ticket_start = info->ticket_start;

	return ts_web_rpc_session_begin(sinfo, *db_id);
}

bool tpp_session_end(int db_id, int ret)
{
	return ts_web_rpc_session_end(db_id, ret);
}

typedef struct TPP_LIB
{
	TPP_LIB()
	{
		dylib = NULL;
		init = NULL;
	}
	~TPP_LIB()
	{
		if (NULL != dylib)
			ex_dlclose(dylib);
		dylib = NULL;
	}

	EX_DYLIB_HANDLE dylib;
	TPP_INIT_FUNC init;
	TPP_START_FUNC start;
	TPP_STOP_FUNC stop;
}TPP_LIB;

typedef std::list<TPP_LIB*> tpp_libs;

class TppManager
{
public:
	TppManager()
	{
	}
	~TppManager()
	{
		tpp_libs::iterator it = m_libs.begin();
		for (; it != m_libs.end(); ++it)
		{
			delete (*it);
		}
		m_libs.clear();
	}

	bool load_tpp(const ex_wstr& libfile);
	void stop_all(void);
	int count(void) { return m_libs.size(); }

private:
	tpp_libs m_libs;
};

static TppManager g_tpp_mgr;
extern ExLogger g_ex_logger;

bool TppManager::load_tpp(const ex_wstr& libname)
{
	ex_wstr filename;
#ifdef EX_OS_WIN32
	filename = libname + L".dll";
#else
    filename = L"lib";
    filename += libname;
	filename += L".so";
#endif

	ex_wstr libfile = g_env.m_exec_path;
	ex_path_join(libfile, false, filename.c_str(), NULL);
	EXLOGV(L"[core] load protocol lib: %ls\n", libfile.c_str());

	TPP_LIB* lib = new TPP_LIB;

	lib->dylib = ex_dlopen(libfile.c_str());
	if (NULL == lib->dylib)
	{
		EXLOGE(L"[core] load dylib `%ls` failed.\n", libfile.c_str());
		delete lib;
		return false;
	}

#ifdef EX_OS_WIN32
	lib->init = (TPP_INIT_FUNC)GetProcAddress(lib->dylib, "tpp_init");
	lib->start = (TPP_START_FUNC)GetProcAddress(lib->dylib, "tpp_start");
	lib->stop = (TPP_STOP_FUNC)GetProcAddress(lib->dylib, "tpp_stop");
#else
    lib->init = (TPP_INIT_FUNC)dlsym(lib->dylib, "tpp_init");
    lib->start = (TPP_START_FUNC)dlsym(lib->dylib, "tpp_start");
    lib->stop = (TPP_STOP_FUNC)dlsym(lib->dylib, "tpp_stop");
#endif

	if (lib->init == NULL || lib->start == NULL || lib->stop == NULL)
	{
		EXLOGE(L"[core] load dylib `%ls` failed, can not locate all functions.\n", libfile.c_str());
		delete lib;
		return false;
	}

	TPP_INIT_ARGS init_args;
	init_args.logger = &g_ex_logger;
	init_args.exec_path = g_env.m_exec_path;
	init_args.etc_path = g_env.m_etc_path;
	init_args.replay_path = g_env.m_replay_path;
	init_args.cfg = &g_env.get_ini();
	init_args.func_take_session = tpp_take_session;
	init_args.func_free_session = tpp_free_session;
	init_args.func_session_begin = tpp_session_begin;
	init_args.func_session_end = tpp_session_end;

	if (EXRV_OK != lib->init(&init_args))
	{
		EXLOGE(L"[core] failed to init protocol `%ls`.\n", libname.c_str());
		delete lib;
		return false;
	}
	if (EXRV_OK != lib->start())
	{
		EXLOGE(L"[core] failed to start protocol `%ls`.\n", libname.c_str());
		delete lib;
		return false;
	}

	m_libs.push_back(lib);
	return true;
}

void TppManager::stop_all(void)
{
	tpp_libs::iterator it = m_libs.begin();
	for (; it != m_libs.end(); ++it)
	{
		(*it)->stop();
	}
}

int ts_main(void)
{
	ExIniFile& ini = g_env.get_ini();

	EXLOGI(L"\n");
	EXLOGI(L"###############################################################\n");
	EXLOGI(L"Load config file: %ls.\n", ini.get_filename().c_str());
	EXLOGI(L"Teleport Core Server starting ...\n");

	ex_ini_sections& secs = ini.GetAllSections();
	TsHttpRpc rpc;

	// 枚举配置文件中的[protocol-xxx]小节，加载对应的协议动态库
	bool all_ok = true;

	do {
		if (!g_session_mgr.start())
		{
			EXLOGE(L"[core] failed to start session-id manager.\n");
			all_ok = false;
			break;
		}

		if (!rpc.init() || !rpc.start())
		{
			EXLOGE(L"[core] rpc init/start failed.\n");
			all_ok = false;
			break;
		}

		ex_ini_sections::iterator it = secs.begin();
		for (; it != secs.end(); ++it)
		{
			if (it->first.length() > 9 && 0 == wcsncmp(it->first.c_str(), L"protocol-", 9))
			{
				ex_wstr libname;
				if (!it->second->GetStr(L"lib", libname))
					continue;

				bool enabled = false;
				it->second->GetBool(L"enabled", enabled, false);
				if (!enabled)
				{
					EXLOGV(L"[core] `%ls` not enabled.\n", libname.c_str());
					continue;
				}

				if (!g_tpp_mgr.load_tpp(libname))
				{
					all_ok = false;
					break;
				}
			}
		}

	} while (0);

	if (0 == g_tpp_mgr.count())
	{
		all_ok = false;
	}

	if (!all_ok)
	{
		g_exit_flag = true;
	}

	if (!g_exit_flag)
	{
		ts_web_rpc_register_core();

		EXLOGV("[core] ---- initialized, ready for service ----\n");
		while (!g_exit_flag)
		{
			ex_sleep_ms(1000);
		}
	}

	g_tpp_mgr.stop_all();
	rpc.stop();
	g_session_mgr.stop();

	return 0;
}
