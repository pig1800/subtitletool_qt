#include "FileTab.h"
#include "AudioData.h"

FileTab::~FileTab()
{
    delete model;
    delete cachedAudioData;
}
