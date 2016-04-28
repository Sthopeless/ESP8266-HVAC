/*
  HVAC.cpp - Arduino library for HVAC control.
  Copyright 2014 Greg Cunningham, CuriousTech.net

  This library is free software; you can redistribute it and/or modify it under the terms of the GNU GPL 2.1 or later.

  This library is distributed in the hope that it will be useful,  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
*/

#include "math.h"
#include "HVAC.h"
#include "Time.h"

#define FF_DELAY 120            // internal furnace fan post-run delay

HVAC::HVAC()
{
  m_EE.size = sizeof(EEConfig);
//----------------------------
  m_EE.cycleMin = 60;         // 60 seconds minimum for a cycle
  m_EE.cycleMax = 60*15;      // 15 minutes maximun for a cycle
  m_EE.idleMin  = 60*5;       // 5 minutes minimum between cycles
  m_EE.cycleThresh =  17;	    // 1.7 degree cycle range
  m_EE.coolTemp[1] = 820;     // 82.0 default temps
  m_EE.coolTemp[0] = 790;     // 79.0
  m_EE.heatTemp[1] = 740;     // 74.0
  m_EE.heatTemp[0] = 700;     // 70.0
  m_EE.eHeatThresh =  30;     // Setting this low (30 deg) for now
  m_EE.fanPostDelay[0] = 60;  // 1 minute after compressor stops (HP)
  m_EE.fanPostDelay[1] = 120; // 2 minutes after compressor stops (cool)
  m_EE.overrideTime = 60*10;  // 10 mins default for override
  m_remoteTimeout   = 60*5;   // 5 minutes default
  m_EE.filterMinutes = 0;
//----------------------------
  memset(m_fcData, -1, sizeof(m_fcData)); // invalidate forecast
  m_outTemp = 0;
  m_inTemp = 0;
  m_rh = 0;
  m_bFanRunning = false;
  m_outMax[0] = -50;      // set as invalid
  m_outMax[1] = -50;	    // set as invalid
  m_bFanMode = false;     // Auto=false, On=true
  m_AutoMode = 0;         // cool, heat
  m_setMode = 0;          // new mode request
  m_setHeat = 0;          // new heat mode request
  m_AutoHeat = 0;         // auto heat mode choice
  m_bRunning = false;     // is operating
  m_bStart = false;       // signal to start
  m_bStop = false;        // signal to stop
  m_bRecheck = false;
  m_bEnabled = false;
  m_runTotal = 0;         // time HVAC has been running total since reset
  m_fanOnTimer = 0;       // time fan is running
  m_cycleTimer = 0;       // time HVAC has been running
  m_fanPostTimer = 0;     // timer for delay
  m_overrideTimer = 0;    // countdown for override in seconds
  m_ovrTemp = 0;          // override delta of target
  m_remoteTimer = 0;      // in seconds
  m_furnaceFan = 0;       // fake fan timer
  m_notif = Note_None;    // Empty
  m_idleTimer = 60*3;     // start with a high idle, in case of power outage
  
  pinMode(P_FAN, OUTPUT);
  pinMode(P_COOL, OUTPUT);
  pinMode(P_REV, OUTPUT);
  pinMode(P_HEAT, OUTPUT);
  digitalWrite(P_HEAT, LOW);
  digitalWrite(P_REV, HIGH); // HIGH = OFF = HEAT
  digitalWrite(P_COOL, LOW);
  digitalWrite(P_FAN, LOW);
}

// Switch the fan on/off
void HVAC::fanSwitch(bool bOn)
{
  if(bOn == m_bFanRunning)
    return;

  digitalWrite(P_FAN, bOn ? HIGH:LOW);
  m_bFanRunning = bOn;
  if(bOn)
  {
    m_fanOnTimer = 0;       // reset fan on timer
  }
}

// Accumulate fan running times
void HVAC::filterInc()
{
  static uint16_t nSecs = 0;
  
  nSecs ++;  // add last run time to total counter
  if(nSecs >= 60)    // increment filter minutes
  {
    m_EE.filterMinutes++;
    nSecs -= 60;     // and subtract a minute
  }
}

// Failsafe: shut everything off
void HVAC::disable()
{
	digitalWrite(P_HEAT, LOW);
	digitalWrite(P_COOL, LOW);
	fanSwitch(false);
	m_bRunning = false;
	m_bEnabled = false;
}

// Service: called once per second
void HVAC::service()
{
  if(m_bFanRunning || m_bRunning || m_furnaceFan)  // furance runs fan seperately
  {
    filterInc();
    if(m_fanOnTimer < 0xFFFF)
      m_fanOnTimer++;			        	// running time counter

    if(m_furnaceFan)                       // fake fan status for furnace fan
      m_furnaceFan--;
  }

  if(m_fanPostTimer)		        		// Fan conintuation delay
  {
    if(--m_fanPostTimer == 0)
      if(!m_bRunning && m_bFanMode == false) // Ensure system isn't running and fanMode is auto
        fanSwitch(false);
  }

  if(m_remoteTimer)                       // remote temperature override timer
    m_remoteTimer--;

  if(m_overrideTimer)       // User temp override timer
  {
    if(--m_overrideTimer == 0)
    {
      m_ovrTemp = 0;
      calcTargetTemp(m_EE.Mode);  // recalc normal set temp
    }
  }

  if(m_bRunning)
  {
    m_runTotal++;
    if(++m_cycleTimer < 20)		        // Block changes for at least 20 seconds after a start
      return;
    if(m_cycleTimer >= m_EE.cycleMax)   // running too long (todo: skip for eHeat?)
    {
      m_bStop = true;
      m_notif = Note_CycleLimit; // cycle limit hit
    }
  }
  else
  {
    m_idleTimer++;                     // Time since stopped
  }

  if(m_setMode != m_EE.Mode || m_setHeat != m_EE.heatMode)   	// requested HVAC mode change
  {
    if(m_bRunning)                      // cycleTimer is already > 20s here
      m_bStop = true;
    if(m_idleTimer >= 5)
    {
      m_EE.heatMode = m_setHeat;
      m_EE.Mode = m_setMode;	        // User may be cycling through modes (give 5s)
      calcTargetTemp(m_EE.Mode);
    }
  }

  int8_t hm = (m_EE.heatMode == Heat_Auto) ? m_AutoHeat : m_EE.heatMode; // true heat mode
  int8_t mode = (m_EE.Mode == Mode_Auto) ? m_AutoMode : m_EE.Mode;      // tue heat/cool mode

  if(m_bStart && !m_bRunning)	            // Start signal occurred
  {
    m_bStart = false;
    
    switch(mode)
    {
      case Mode_Cool:
        fanSwitch(true);
        if(digitalRead(P_REV) != LOW)
        {
          digitalWrite(P_REV, LOW);  // set heatpump to cool (if heats, reverse this)
          delay(3000);               //    if no heatpump, remove
        }
        digitalWrite(P_COOL, HIGH);
        break;
    case Mode_Heat:
        if(hm)  // gas
        {
          digitalWrite(P_HEAT, HIGH);
        }
        else
        {
          fanSwitch(true);
          if(digitalRead(P_REV) != HIGH)  // set heatpump to heat (if heats, reverse this)
          {
            digitalWrite(P_REV, HIGH);
            delay(3000);
          }
          digitalWrite(P_COOL, HIGH);
        }
        break;
    }
    m_bRunning = true;
    m_cycleTimer = 0;
  }

  if(m_bStop && m_bRunning)	        		// Stop signal occurred
  {
    m_bStop = false;
    digitalWrite(P_COOL, LOW);
    digitalWrite(P_HEAT, LOW);
  
    if(m_bFanRunning && m_bFanMode == false) // Note: furance manages fan
    {
      if(m_EE.fanPostDelay[digitalRead(P_REV)])			    // leave fan running to circulate air longer
        m_fanPostTimer = m_EE.fanPostDelay[digitalRead(P_REV)]; // P_REV == true if heating
      else
        fanSwitch(false);
    }
  
    if(mode == Mode_Heat && hm)   // count run time as fan time in winter
    {                             // furnace post fan is 120 seconds
      m_furnaceFan = FF_DELAY;
    }

    m_bRunning = false;
    m_idleTimer = 0;
  }

  tempCheck();
}

bool HVAC::stateChange()
{
   static bool bFan = false;
   static uint8_t lastMode = 0;
   static uint8_t nState = 0;

   if(getMode() != lastMode || getState() != nState || bFan != getFanRunning())   // erase prev highlight
   {
       lastMode = getMode();
       nState = getState();
       bFan = getFanRunning();
       return true;
   }
   return false;
}

// Control switching of system by temp
void HVAC::tempCheck()
{
  if(m_inTemp == 0 || m_bEnabled == false)   	// hasn't been set yet
    return;

  if(m_EE.Mode == Mode_Off)		// nothing to do
    return;

  if(m_bRunning)
  {
    if(m_cycleTimer < m_EE.cycleMin)
      return;

    int8_t mode = (m_EE.Mode == Mode_Auto) ? m_AutoMode : m_EE.Mode;

    if(second() == 0 || m_bRecheck) // readjust while running
    {
      m_bRecheck = false;
      preCalcCycle(m_EE.Mode);
    }

    switch(mode)
    {
      case Mode_Cool:
        if( m_inTemp <= m_targetTemp - m_EE.cycleThresh ) // has cooled to desired temp - threshold
          m_bStop = true;
        break;
      case Mode_Heat:
        if(m_inTemp > m_targetTemp + m_EE.cycleThresh) // has heated above desired temp + threshold
          m_bStop = true;
        break;
    }
  }
  else	// not running
  {
    if(m_idleTimer < m_EE.idleMin)
      return;

    if(second() == 0 || m_bRecheck)
    {
      m_bRecheck = false;
      m_bStart = preCalcCycle(m_EE.Mode);
    }
  }
}

bool HVAC::preCalcCycle(int8_t mode)
{
  bool bRet = false;
	
  // Standard triggers for now
  switch(mode)
  {
    case Mode_Cool:
      calcTargetTemp(Mode_Cool);
      bRet = (m_inTemp >= m_targetTemp);    // has reached threshold above desired temp
      break;
    case Mode_Heat:
      calcTargetTemp(Mode_Heat);
      bRet = (m_inTemp <= m_targetTemp);
      break;
    case Mode_Auto:
      if(m_inTemp >= m_EE.coolTemp[0])
      {
        calcTargetTemp(Mode_Cool);
        m_AutoMode = Mode_Cool;
        bRet = (m_inTemp >= m_targetTemp);    // has reached threshold above desired temp
      }
      else if(m_inTemp <= m_EE.heatTemp[1])
      {
//      Serial.println("Auto heat");
        m_AutoMode = Mode_Heat;
        calcTargetTemp(Mode_Heat);
        if(m_EE.heatMode == Heat_Auto)
        {
          if(m_inTemp < m_outTemp - (m_EE.eHeatThresh * 10)) 	// Use gas when efficiency too low for pump  Todo: change this to outtemp threshold
            m_AutoHeat = Heat_NG;
          else
            m_AutoHeat = Heat_HP;
        }
        bRet = (m_inTemp <= m_targetTemp);
      }
      break;
  }
  return bRet;
}

void HVAC::calcTargetTemp(int8_t mode)
{
  if(!m_bRunning)
    if(digitalRead(P_REV) != (mode == Mode_Heat) )  // set heatpump same as current mode (Note: some models may be reversed)
      digitalWrite(P_REV, (mode == Mode_Heat) ? HIGH : LOW);

  int16_t L = m_outMin[1];
  int16_t H = m_outMax[1];

  if(m_outMax[0] != -50)  // Use longer range if available
  {
    L = min(m_outMin[0], L);
    H = max(m_outMax[0], H);
  }

  L *= 10;    // shift a decimal place
  H *= 10;

  switch(mode)
  {
    case Mode_Cool:
      m_targetTemp  = (m_outTemp-L) * (m_EE.coolTemp[1]-m_EE.coolTemp[0]) / (H-L) + m_EE.coolTemp[0];
      m_targetTemp = constrain(m_targetTemp, m_EE.coolTemp[0], m_EE.coolTemp[1]); // just for safety
      break;
    case Mode_Heat:
      m_targetTemp  = (m_outTemp-L) * (m_EE.heatTemp[1]-m_EE.heatTemp[0]) / (H-L) + m_EE.heatTemp[0];
      m_targetTemp = constrain(m_targetTemp, m_EE.heatTemp[0], m_EE.heatTemp[1]); // just for safety
      break;
  }
  m_targetTemp += m_ovrTemp; // override is normally 0, unless set remotely with a timeout
//	Serial.print(" target=");
//	Serial.println(m_targetTemp);
}

uint8_t HVAC::getState()
{
  if( m_bRunning == false) return 0;

  // Check if NG furnace is running, which controls the fan automatically
  uint8_t state = (m_EE.Mode == Mode_Auto) ? m_AutoMode : m_EE.Mode; // convert auto to just cool / heat

  if(state == Mode_Heat && ( m_EE.heatMode == Heat_NG || (m_EE.heatMode == Heat_Auto && m_AutoHeat == Heat_NG) ) )  // convert any NG mode to 3
    state = 3; // so logs will only be 1, 2 or 3.

  return state;
}

bool HVAC::getFanRunning()
{
  return (m_bRunning || m_furnaceFan || m_bFanRunning);
}

/* this seems overdone
bool HVAC::getFanRunning()
{
  bool bOn = m_bFanRunning;

  uint8_t mode = (m_EE.Mode == Mode_Auto) ? m_AutoMode : m_EE.Mode; // convert auto to just cool / heat

  // Check if NG furnace is running, which controls the fan automatically
  if(mode == Mode_Heat && ( m_EE.heatMode == Heat_NG || (m_EE.heatMode == Heat_Auto && m_AutoHeat == Heat_NG) )) // if heat is nat gas
  {
    if(m_bRunning || m_furnaceFan)
      bOn = true;
  }
  return bOn;
}
*/

uint8_t HVAC::getMode()
{
  return m_EE.Mode;
}

void HVAC::setHeatMode(uint8_t mode)
{
  m_setHeat = mode % 3;
}

uint8_t HVAC::getHeatMode()
{
  return m_EE.heatMode;
}

int8_t HVAC::getAutoMode()
{
  return m_AutoMode;
}

int8_t HVAC::getSetMode()
{
  return m_setMode;
}

// User:Set a new control mode
void HVAC::setMode(int8_t mode)
{
  m_setMode = mode & 3;
  if(!m_bRunning)
  {
     if(m_idleTimer < m_EE.idleMin - 30)
 	     m_idleTimer = m_EE.idleMin - 10;        // keep it from starting too quickly, but not too long
  }
}

void HVAC::enable()
{
  m_bEnabled = true;
  m_bRecheck = true;
}

bool HVAC::getFan()
{
  return m_bFanMode;
}

// User:Set fan mode
void HVAC::setFan(bool bon)
{
  if(bon == m_bFanMode)			// requested fan operating mode change
    return;

  m_bFanMode = bon;
  if(!m_bRunning)
    fanSwitch(bon);              // manual fan on/off if not running
}

int16_t HVAC::getSetTemp(int8_t mode, int8_t hl)
{
  switch(mode)
  {
    case Mode_Cool:
      return m_EE.coolTemp[hl];
    case Mode_Heat:
      return m_EE.heatTemp[hl];
    case Mode_Auto:
      return (m_AutoMode == Mode_Cool) ? m_EE.coolTemp[hl] : m_EE.heatTemp[hl];
  }
  return 0;
}

// User:Set new control temp
void HVAC::setTemp(int8_t mode, int16_t Temp, int8_t hl)
{
  if(mode == Mode_Auto)
  {
    mode = m_AutoMode;
  }

  int8_t save;

  switch(mode)
  {
    case Mode_Cool:
      if(Temp < 650 || Temp > 880)    // ensure sane values
        break;
      m_EE.coolTemp[hl] = Temp;
      if(hl)
      {
        m_EE.coolTemp[0] = min(m_EE.coolTemp[1], m_EE.coolTemp[0]);     // don't allow h/l to invert
      }
      else
      {
        m_EE.coolTemp[1] = max(m_EE.coolTemp[0], m_EE.coolTemp[1]);
      }
      save = m_EE.heatTemp[1] - m_EE.heatTemp[0];
      m_EE.heatTemp[1] = min(m_EE.coolTemp[0] - 20, m_EE.heatTemp[1]); // Keep 2.0 degree differential for Auto mode
      m_EE.heatTemp[0] = m_EE.heatTemp[1] - save;                      // shift heat low by original diff

      if(m_EE.Mode == Mode_Cool)
        calcTargetTemp(m_EE.Mode);

      break;
    case Mode_Heat:
      if(Temp < 630 || Temp > 860)    // ensure sane values
        break;
      m_EE.heatTemp[hl] = Temp;
      if(hl)
      {
        m_EE.heatTemp[0] = min(m_EE.heatTemp[1], m_EE.heatTemp[0]);
      }
      else
      {
        m_EE.heatTemp[1] = max(m_EE.heatTemp[0], m_EE.heatTemp[1]);
      }
      save = m_EE.coolTemp[1] - m_EE.coolTemp[0];
      m_EE.coolTemp[0] = max(m_EE.heatTemp[1] - 20, m_EE.coolTemp[0]);
      m_EE.coolTemp[1] = m_EE.coolTemp[0] + save;

      if(m_EE.Mode == Mode_Heat)
        calcTargetTemp(m_EE.Mode);

      break;
  }
}

bool HVAC::isRemoteTemp()
{
  return m_remoteTimer ? true:false;
}

// Update when DHT22/SHT21 changes
void HVAC::updateIndoorTemp(int16_t Temp, int16_t rh)
{
	if(m_remoteTimer == 0)	// only get local temp if no remote
    	m_inTemp = Temp;
	m_rh = rh;
}

// Update outdoor temp
void HVAC::updateOutdoorTemp(int16_t outTemp)
{
	m_outTemp = outTemp;
}

// Update min/max for next 24 hrs
void HVAC::updatePeaks(int8_t min, int8_t max)
{
	if(m_outMax[0] != -50)      // preserve peaks longer
	{
		m_outMax[0] = m_outMax[1];
		m_outMin[0] = m_outMin[1];
	}
	else                        // initial value
	{
		m_outMin[0] = min;
		m_outMax[0] = max;
	}
	
	m_outMin[1] = min;
	m_outMax[1] = max;
}

void HVAC::resetFilter()
{
	m_EE.filterMinutes = 0;
  if(m_notif == Note_Filter)
    m_notif = Note_None;
}

// returns filter over 200 hours
bool HVAC::checkFilter(void)
{
	return (m_EE.filterMinutes >= 60*200);
}

void HVAC::resetTotal()
{
	m_runTotal = 0;
}

// Current control settings
String HVAC::settingsJson()
{
    String s = "{";
    s += "\"m\":";   s += m_EE.Mode;
    s += ",\"am\":";  s += m_AutoMode;
    s += ",\"hm\":";  s += m_EE.heatMode;
    s += ",\"fm\":";  s += m_bFanMode;
    s += ",\"ot\":";  s += m_ovrTemp;
    s += ",\"ht\":";  s += m_EE.eHeatThresh;
    s += ",\"c0\":";  s += m_EE.coolTemp[0];
    s += ",\"c1\":";  s += m_EE.coolTemp[1];
    s += ",\"h0\":";  s += m_EE.heatTemp[0];
    s += ",\"h1\":";  s += m_EE.heatTemp[1];
    s += ",\"im\":";  s += m_EE.idleMin;
    s += ",\"cn\":";  s += m_EE.cycleMin;
    s += ",\"cx\":";  s += m_EE.cycleMax;
    s += ",\"ct\":";  s += m_EE.cycleThresh;
    s += ",\"fd\":";  s += m_EE.fanPostDelay[digitalRead(P_REV)];
    s += ",\"ov\":";  s += m_EE.overrideTime;
    s += "}";
    return s;
}

// Constant changing values
String HVAC::getPushData()
{
    String s = "{";
    s += "\"r\":" ;  s += m_bRunning;
    s += ",\"fr\":";  s += getFanRunning(),
    s += ",\"s\":" ;  s += getState();
    s += ",\"it\":";  s += m_inTemp;
    s += ",\"rh\":";  s += m_rh;
    s += ",\"tt\":";  s += m_targetTemp;
    s += ",\"fm\":";  s += m_EE.filterMinutes;
    s += ",\"ot\":";  s += m_outTemp;
    s += ",\"ol\":";  s += m_outMin[1];
    s += ",\"oh\":";  s += m_outMax[1];
    s += ",\"ct\":";  s += m_cycleTimer;
    s += ",\"ft\":";  s += m_fanOnTimer;
    s += ",\"rt\":";  s += m_runTotal;
    s += "}";
    return s;
}

static const char *cSCmds[] =
{
	"fanmode",
	"mode",
	"heatmode",
	"resettotal",
	"resetfilter",
	"fanpostdelay",
	"cyclemin",
	"cyclemax",
	"idlemin",
	"cyclethresh",
	"cooltempl",
	"cooltemph",
	"heattempl",
	"heattemph",
	"eheatthresh",
	"override",
	"overridetime",
	"remotetemp",
	"remotetime",
	NULL
};

int HVAC::CmdIdx(String s, const char **pCmds )
{
  int iCmd;
  
  for(iCmd = 0; pCmds[iCmd]; iCmd++)
  {
    if( s.equalsIgnoreCase( String(pCmds[iCmd]) ) )
      break;
  }
  return iCmd;
}

// POST set params as "set: fanmode=1"
void HVAC::setVar(String sCmd, int val)
{
	switch( CmdIdx( sCmd, cSCmds ) )
	{
		case 0:     // fanmode
			setFan( (val) ? true:false);
      break;
		case 1:     // mode
			setMode( val );
      break;
		case 2:     // heatmode
			setHeatMode( val );
      break;
		case 3:     // resettotal
			resetTotal();
			break;
		case 4:
			resetFilter();
      break;
		case 5:     // fanpostdelay
			m_EE.fanPostDelay[digitalRead(P_REV)] = constrain(val, 0, 60*5); // Limit 0 to 5 minutes
			break;
		case 6:     // cyclemin
			m_EE.cycleMin = constrain(val, 60, 60*20); // Limit 1 to 20 minutes
			break;
		case 7:     // cyclemax
			m_EE.cycleMax = constrain(val, 60*2, 60*60); // Limit 2 to 60 minutes
			break;
		case 8:     // idlemin
			m_EE.idleMin = constrain(val, 60, 60*30); // Limit 1 to 30 minutes
			break;
		case 9:    // cyclethresh
			m_EE.cycleThresh = constrain(val, 5, 50); // Limit 0.5 to 5.0 degrees
			break;
		case 10:    // cooltempl
			setTemp(Mode_Cool, val, 0);
			m_bRecheck = true; // faster update
      break;
		case 11:    // cooltemph
			setTemp(Mode_Cool, val, 1);
			m_bRecheck = true;
      break;
		case 12:    // heattempl
			setTemp(Mode_Heat, val, 0);
			m_bRecheck = true;
      break;
		case 13:    // heattemph
			setTemp(Mode_Heat, val, 1);
			m_bRecheck = true;
      break;
		case 14:    // eheatthresh
			m_EE.eHeatThresh = constrain(val, 5, 50); // Limit 5 to 50 degrees F
			break;
		case 15:    // override
			if(val <= 0)    // cancel
			{
        m_ovrTemp = 0;
				m_overrideTimer = 0;
				m_bRecheck = true;
			}
			else
			{
				m_ovrTemp = constrain(val, -90, 90); // Limit to -9.0 to +9.0 degrees F
				m_overrideTimer = m_EE.overrideTime;
				m_bRecheck = true;
			}
			break;
		case 16:    // overridetime
			m_EE.overrideTime = constrain(val, 60*1, 60*60*5); // Limit 1 min to 5 hours
			break;
		case 17: // remotetemp  (must be updated every "remotetime" interval or less or it will switch back to internal temperature)
      if(val > 0)
      {
         m_inTemp = constrain(val, 650, 880); // Limit 65 to 88 degrees F
         m_remoteTimer = m_remoteTimeout;    // heartbeat
      }
      else
      {
         m_remoteTimer = 0;  // temp = 0 to cancel
      }
      break;
		case 18: // remotetime
      m_remoteTimeout = constrain(val, 1, 60*5); // Limit 1 sec to 5 minutes
		  break;
	}
}