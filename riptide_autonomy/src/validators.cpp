#include "riptide_autonomy/validators.h"

DetectionValidator::DetectionValidator(int detections, double duration)
{
  durationThresh = duration;
  detsReq = detections;
  Reset();
}

bool DetectionValidator::Validate()
{
  if (++detections == 1) {
    startTime = ros::Time::now();
  }

  if (ros::Time::now().toSec() - startTime.toSec() > durationThresh) {
    valid = detections >= detsReq;
    detections = 0;
    attempts++;
  }

  return valid;
}

bool DetectionValidator::IsValid()
{
  return valid;
}

int DetectionValidator::GetDetections()
{
  return detections;
}

int DetectionValidator::GetAttempts()
{
  return attempts;
}

void DetectionValidator::Reset()
{
  valid = false;
  detections = 0;
  attempts = 0;
}

ErrorValidator::ErrorValidator(double errorThresh, double duration)
{
  durationThresh = duration;
  this->errorThresh = errorThresh;
  Reset();
}

bool ErrorValidator::Validate(double error)
{
  if (abs(error) <= errorThresh)
  {
    if (outsideRange)
      startTime = ros::Time::now();

    outsideRange = false;

    return ros::Time::now().toSec() - startTime.toSec() > durationThresh;
  }
  else
    outsideRange = true;

  return false;
}

// FIX: This will not always mean the error is valid if you only look at the time difference
bool ErrorValidator::IsValid()
{
  return ros::Time::now().toSec() - startTime.toSec() > durationThresh;
}

void ErrorValidator::Reset()
{
  outsideRange = true;
}
