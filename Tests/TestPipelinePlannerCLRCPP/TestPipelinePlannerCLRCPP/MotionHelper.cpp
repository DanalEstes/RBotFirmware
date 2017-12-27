// RBotFirmware
// Rob Dobson 2016

#include "application.h"
#include "ConfigPinMap.h"
#include "math.h"
#include "MotionHelper.h"
#include "Utils.h"

MotionHelper::MotionHelper() :
		_motionActuator(_motionIO, _motionPipeline)
{
	// Init
	_isPaused = false;
	_moveRelative = false;
    _xMaxMM = 0;
    _yMaxMM = 0;
	_blockDistanceMM = 0;
	// Clear axis current location
	_curAxisPosition.clear();
	// Coordinate conversion management
	_ptToActuatorFn = NULL;
	_actuatorToPtFn = NULL;
	_correctStepOverflowFn = NULL;
}

MotionHelper::~MotionHelper()
{
}

void MotionHelper::setTransforms(ptToActuatorFnType ptToActuatorFn, actuatorToPtFnType actuatorToPtFn,
	correctStepOverflowFnType correctStepOverflowFn)
{
	// Store callbacks
	_ptToActuatorFn = ptToActuatorFn;
	_actuatorToPtFn = actuatorToPtFn;
	_correctStepOverflowFn = correctStepOverflowFn;
}

void MotionHelper::configure(const char* robotConfigJSON)
{
	// MotionIO
	_motionIO.deinit();

	// Configure Axes
	_axesParams.clearAxes();
	String axisJSON;
	for (int axisIdx = 0; axisIdx < RobotConsts::MAX_AXES; axisIdx++)
	{
		if (_axesParams.configureAxis(robotConfigJSON, axisIdx, axisJSON))
		{
			// Configure motionIO - motors and end-stops
			_motionIO.configureAxis(axisJSON.c_str(), axisIdx);
		}
	}

	// Configure robot
	configureRobot(robotConfigJSON);

	// Clear motion info
	_curAxisPosition.clear();
}

bool MotionHelper::configureRobot(const char* robotConfigJSON)
{
	// Get motor enable info
	String stepEnablePinName = RdJson::getString("stepEnablePin", "-1", robotConfigJSON);
	_xMaxMM = float(RdJson::getDouble("xMaxMM", 0, robotConfigJSON));
	_yMaxMM = float(RdJson::getDouble("yMaxMM", 0, robotConfigJSON));

	// Motion Pipeline and Planner
	_blockDistanceMM = float(RdJson::getDouble("blockDistanceMM", blockDistanceMM_default, robotConfigJSON));
	float junctionDeviation = float(RdJson::getDouble("junctionDeviation", junctionDeviation_default, robotConfigJSON));
	int pipelineLen = int(RdJson::getLong("pipelineLen", pipelineLen_default, robotConfigJSON));
	// Pipeline length and block size
	Log.trace("MotionHelper configMotionPipeline len %d, _blockDistanceMM %0.2f",
					pipelineLen, _blockDistanceMM);
	_motionPipeline.init(pipelineLen);
	_motionPlanner.configure(junctionDeviation);

	// MotionIO
	_motionIO.configureMotors(robotConfigJSON);
	return true;
}
#
bool MotionHelper::canAccept()
{
	// Check that at the motion pipeline can accept new data
	return _motionPipeline.canAccept();
}

// Pause (or un-pause) all motion
void MotionHelper::pause(bool pauseIt)
{
	_motionActuator.pause(pauseIt);
	_isPaused = pauseIt;
}

// Check if paused
bool MotionHelper::isPaused()
{
	return _isPaused;
}

// Stop
void MotionHelper::stop()
{
	_motionPipeline.clear();
	_motionActuator.clear();
	_isPaused = false;
}

// Check if idle
bool MotionHelper::isIdle()
{
	return !_motionPipeline.canGet();
}

void MotionHelper::setMotionParams(RobotCommandArgs& args)
{
	// Check for relative movement specified and set accordingly
	if (args.moveType != RobotMoveTypeArg_None)
		_moveRelative = (args.moveType == RobotMoveTypeArg_Relative);
}

void MotionHelper::getCurStatus(RobotCommandArgs& args)
{
	// Get current position
	AxisFloats axisPosns;
	args.pt = _curAxisPosition._axisPositionMM;
	// Absolute/Relative movement
	args.moveType = _moveRelative ? RobotMoveTypeArg_Relative : RobotMoveTypeArg_Absolute;
}

bool MotionHelper::moveTo(RobotCommandArgs& args)
{
	Log.trace("+++++++MotionHelper moveTo x %0.2f y %0.2f", args.pt._pt[0], args.pt._pt[1]);

	// Handle any motion parameters (such as relative movement, feedrate, etc)
	setMotionParams(args);

	// Create a motion element for this movement
	MotionElem elem(_curAxisPosition._axisPositionMM, args.pt);

	// Convert the move to actuator coordinates
	AxisFloats actuatorCoords;
	_ptToActuatorFn(elem, actuatorCoords, _axesParams.getAxisParamsArray(), RobotConsts::MAX_AXES);

	// Plan the move
	bool moveOk = _motionPlanner.moveTo(args, elem, actuatorCoords, _curAxisPosition, _axesParams, _motionPipeline);
	if (moveOk)
	{
		// Update axisMotion
		_curAxisPosition._axisPositionMM = args.pt;
		_curAxisPosition._stepsFromHome = actuatorCoords;
	}
	return moveOk;

}

void MotionHelper::service(bool processPipeline)
{
	// Check if we should process the movement pipeline
	if (processPipeline)
	{
		_motionActuator.processSmoothie();
	}
}

void MotionHelper::pipelineService(bool hasBeenPaused)
{
	//// Check if any axis is moving
	//bool anyAxisMoving = false;
	//for (int i = 0; i < RobotConsts::MAX_AXES; i++)
	//{
	//	// Check if movement required
	//	if (_axisParams[i]._targetStepsFromHome == _axisParams[i]._stepsFromHome)
	//		continue;
	//	anyAxisMoving = true;
	//}

	//// If nothing moving then prep the next pipeline element (if there is one)
	//if (!anyAxisMoving)
	//{
	//	MotionPipelineElem motionElem;
	//	if (_motionPipeline.get(motionElem))
	//	{
	//		// Correct for any overflows in stepper values (may occur with rotational robots)
	//		_correctStepOverflowFn(_axisParams, _numRobotAxes);

	//		// Check if a real distance
	//		double distToTravelMM = motionElem.delta();
	//		bool valid = true;
	//		if (distToTravelMM < distToTravelMM_ignoreBelow)
	//			valid = false;

	//		// Get absolute step position to move to
	//		PointND actuatorCoords;
	//		if (valid)
	//			valid = _ptToActuatorFn(motionElem, actuatorCoords, _axisParams, _numRobotAxes);

	//		// Activate motion if valid - otherwise ignore
	//		if (valid)
	//		{
	//			// Get steps from home for each axis
	//			for (int i = 0; i < RobotConsts::MAX_AXES; i++)
	//				_axisParams[i]._targetStepsFromHome = actuatorCoords.getVal(i);

	//			// Balance the time for each direction
	//			// double calcMotionTime = calcMotionTimeUs(motionElem, axisParams);
	//			double speedTargetMMps = _axisParams[0]._maxSpeed;
	//			double timeToTargetS = distToTravelMM / speedTargetMMps;
	//			long stepsAxis[RobotConsts::MAX_AXES];
	//			double timePerStepAxisNs[RobotConsts::MAX_AXES];
	//			Log.trace("motionHelper speedTargetMMps %0.2f distToTravelMM %0.2f timeToTargetMS %0.2f",
	//				speedTargetMMps, distToTravelMM, timeToTargetS*1000.0);
	//			for (int i = 0; i < RobotConsts::MAX_AXES; i++)
	//			{
	//				stepsAxis[i] = labs(_axisParams[i]._targetStepsFromHome - _axisParams[i]._stepsFromHome);
	//				if (stepsAxis[i] == 0)
	//					timePerStepAxisNs[i] = 1000000000;
	//				else
	//					timePerStepAxisNs[i] = timeToTargetS * 1000000000 / stepsAxis[i];
	//				if (timePerStepAxisNs[i] < _axisParams[i]._minNsBetweenSteps)
	//					timePerStepAxisNs[i] = _axisParams[i]._minNsBetweenSteps;
	//				Log.trace("motionHelper axis%d target %ld fromHome %ld timerPerStepNs %f",
	//					i, _axisParams[i]._targetStepsFromHome, _axisParams[i]._stepsFromHome,
	//					timePerStepAxisNs[i]);
	//			}

	//			// Check if there is little difference from current timePerStep on the
	//			// dominant axis
	//			if (_axisParams[0]._isDominantAxis && isInBounds(timePerStepAxisNs[0],
	//				_axisParams[0]._betweenStepsNs*0.66, _axisParams[0]._betweenStepsNs*1.33))
	//			{
	//				// In that case use the dominant axis time with a correction factor
	//				double speedCorrectionFactor = _axisParams[0]._betweenStepsNs / timePerStepAxisNs[0];
	//				_axisParams[1]._betweenStepsNs = timePerStepAxisNs[1] * speedCorrectionFactor;
	//			}
	//			else if (_axisParams[1]._isDominantAxis && isInBounds(timePerStepAxisNs[1],
	//				_axisParams[1]._betweenStepsNs*0.66, _axisParams[1]._betweenStepsNs*1.33))
	//			{
	//				// In that case use the dominant axis time with a correction factor
	//				double speedCorrectionFactor = _axisParams[1]._betweenStepsNs / timePerStepAxisNs[1];
	//				_axisParams[0]._betweenStepsNs = timePerStepAxisNs[0] * speedCorrectionFactor;
	//			}
	//			else
	//			{
	//				// allow each axis to use a different stepping time
	//				_axisParams[0]._betweenStepsNs = timePerStepAxisNs[0];
	//				_axisParams[1]._betweenStepsNs = timePerStepAxisNs[1];
	//			}
	//		}

	//		// Debug
	//		Log.info("MotionHelper StepNS X %ld Y %ld Z %ld)", _axisParams[0]._betweenStepsNs,
	//			_axisParams[1]._betweenStepsNs, _axisParams[2]._betweenStepsNs);
	//		motionElem._pt1MM.logDebugStr("MotionFrom");
	//		motionElem._pt2MM.logDebugStr("MotionTo");

	//		// Log.trace("Move to %sx %0.2f y %0.2f -> ax0Tgt %0.2f Ax1Tgt %0.2f (stpNSXY %ld %ld)",
	//		//             valid?"":"INVALID ", motionElem._pt2MM._pt[0], motionElem._pt2MM._pt[1],
	//		//             actuatorCoords._pt[0], actuatorCoords._pt[1],
	//		//             _axisParams[0]._betweenStepsNs, _axisParams[1]._betweenStepsNs);
	//	}
	//}

	//// Make the next step on each axis as requred
	//for (int i = 0; i < RobotConsts::MAX_AXES; i++)
	//{
	//	// Check if a move is required
	//	if (_axisParams[i]._targetStepsFromHome == _axisParams[i]._stepsFromHome)
	//		continue;

	//	// Check for servo driven axis (doesn't need individually stepping)
	//	if (_axisParams[i]._isServoAxis)
	//	{
	//		Log.trace("Servo(ax#%d) jump to %ld", i, _axisParams[i]._targetStepsFromHome);
	//		jump(i, _axisParams[i]._targetStepsFromHome);
	//		_axisParams[i]._stepsFromHome = _axisParams[i]._targetStepsFromHome;
	//		continue;
	//	}

	//	//// Check if time to move
	//	//if (hasBeenPaused || (Utils::isTimeout(micros(), _axisParams[i]._lastStepMicros, _axisParams[i]._betweenStepsNs / 1000)))
	//	//{
	//	//	step(i, _axisParams[i]._targetStepsFromHome > _axisParams[i]._stepsFromHome);
	//	//	// Log.trace("Step %d %d", i, _axisParams[i]._targetStepsFromHome > _axisParams[i]._stepsFromHome);
	//	//	_axisParams[i]._betweenStepsNs += _axisParams[i]._betweenStepsNsChangePerStep;
	//	//}
	//}

}

//bool MotionHelper::isMoving()
//{
//	for (int i = 0; i < RobotConsts::MAX_AXES; i++)
//	{
//		// Check if movement required - if so we are busy
//		if (_axisParams[i]._targetStepsFromHome != _axisParams[i]._stepsFromHome)
//			return true;
//	}
//	return false;
//}

//void MotionHelper::axisSetHome(int axisIdx)
//{
//	if (axisIdx < 0 || axisIdx >= RobotConsts::MAX_AXES)
//		return;
//	if (_axisParams[axisIdx]._isServoAxis)
//	{
//		_axisParams[axisIdx]._homeOffsetSteps = _axisParams[axisIdx]._stepsFromHome;
//	}
//	else
//	{
//		_axisParams[axisIdx]._stepsFromHome = _axisParams[axisIdx]._homeOffsetSteps;
//		_axisParams[axisIdx]._targetStepsFromHome = _axisParams[axisIdx]._homeOffsetSteps;
//	}
//	Log.trace("Setting axis#%d home %lu", axisIdx, _axisParams[axisIdx]._homeOffsetSteps);
//#ifdef USE_MOTION_ISR_MANAGER
//	motionISRManager.resetZero(axisIdx);
//#endif
//}

void MotionHelper::debugShowBlocks()
{
	int elIdx = 0;
	bool headShown = false;
	for (int i = _motionPipeline.count() - 1; i >= 0; i--)
	{
		MotionBlock* pBlock = _motionPipeline.peekNthFromPut(i);
		if (pBlock)
		{
			if (!headShown)
			{
				pBlock->debugShowBlkHead();
				headShown = true;
			}
			pBlock->debugShowBlock(elIdx++);
		}
	}
}

int MotionHelper::testGetPipelineCount()
{
	return _motionPipeline.count();
}
void MotionHelper::testGetPipelineBlock(int elIdx, MotionBlock& block)
{
	block = *_motionPipeline.peekNthFromPut(_motionPipeline.count() - 1 - elIdx);
}
