/*
 * Copyright (C) 2012 Alexandr Vodiannikov aka "Aleksoid1978" (Aleksoid1978@mail.ru)
 *
 * This file is part of WinnerMediaPlayer.
 *
 * WinnerMediaPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinnerMediaPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "WMPShellExt_i.h"
#include "dllmain.h"

CWMPShellExtModule _AtlModule;

class CWMPShellExtApp : public CWinApp
{
public:

// Overrides
	virtual BOOL InitInstance();
	virtual int ExitInstance();

	DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CWMPShellExtApp, CWinApp)
END_MESSAGE_MAP()

CWMPShellExtApp theApp;

BOOL CWMPShellExtApp::InitInstance()
{
	return CWinApp::InitInstance();
}

int CWMPShellExtApp::ExitInstance()
{
	return CWinApp::ExitInstance();
}
