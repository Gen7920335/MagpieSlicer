
var TargetPage=null;
var ProfileReady=false;
var Navigated=false;
var StatusTimer=null;
// Maximum time waiting for the native profile loader, in milliseconds.
var ProfileWaitLimitMs=180*1000;
var LoadStartedMs=0;
var LoadError=null;

function RequestLoadStatus()
{
	if(Navigated) return;
	if(Date.now()-LoadStartedMs >= ProfileWaitLimitMs) {
		ShowLoadError('Profile loading timed out. Close this window and try again.');
		return;
	}
	// Schedule before sending: the native bridge may respond synchronously.
	StatusTimer=setTimeout(RequestLoadStatus,1000);
	SendWXMessage(JSON.stringify({command:'request_userguide_load_status'}));
}

function ShowLoadError(message)
{
	LoadError=message;
	clearTimeout(StatusTimer);
	var tip=document.getElementById('LoadTip');
	if(tip) tip.textContent=message;
}

function OnInit()
{
	TranslatePage();

	TargetPage=GetQueryString("target");
	LoadStartedMs=Date.now();
	if(ProfileReady) JumpToTarget();
	else if(LoadError) ShowLoadError(LoadError);
	else RequestLoadStatus();
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];
	
	if(strCmd=='userguide_profile_load_finish')
	{
		ProfileReady=true;
		JumpToTarget();
	}
	else if(strCmd=='userguide_profile_load_error')
		ShowLoadError(pVal['message']);
}

function JumpToTarget()
{
	if(!ProfileReady || TargetPage===null || Navigated) return;
	if(!/^(1|11|21|22|23|24)$/.test(TargetPage)) {
		ShowLoadError('Invalid setup page. Close this window and try again.');
		return;
	}
	Navigated=true;
	clearTimeout(StatusTimer);
	window.open('../'+TargetPage+'/index.html','_self');
}
