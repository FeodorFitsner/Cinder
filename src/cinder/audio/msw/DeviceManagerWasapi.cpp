/*
 Copyright (c) 2014, The Cinder Project

 This code is intended to be used with the Cinder C++ library, http://libcinder.org

 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and
	the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
	the following disclaimer in the documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
*/

#include "cinder/Cinder.h"

#if( _WIN32_WINNT >= 0x0600 ) // Requires Windows Vista+

#include "cinder/audio/msw/DeviceManagerWasapi.h"
#include "cinder/audio/msw/MswUtil.h"
#include "cinder/CinderAssert.h"
#include "cinder/Log.h"
#include "cinder/msw/CinderMsw.h"

#include <initguid.h> // must be included before mmdeviceapi.h for the pkey defines to be properly instantiated. Both must be first included from a translation unit.
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Audioclient.h>

#define ASSERT_HR_OK( hr ) CI_ASSERT_MSG( hr == S_OK, hresultToString( hr ) )

using namespace std;

namespace cinder { namespace audio { namespace msw {

// ----------------------------------------------------------------------------------------------------
// MARK: - DeviceManagerWasapi
// ----------------------------------------------------------------------------------------------------

DeviceRef DeviceManagerWasapi::getDefaultOutput()
{
	::IMMDeviceEnumerator *enumerator;
	HRESULT hr = ::CoCreateInstance( __uuidof(::MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(::IMMDeviceEnumerator), (void**)&enumerator );
	CI_ASSERT( hr == S_OK );
	auto enumeratorPtr = ci::msw::makeComUnique( enumerator );

	::IMMDevice *device;
	hr = enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device );
	if( hr == E_NOTFOUND ) {
		return nullptr; // no device available
	}
	CI_ASSERT( hr == S_OK );

	auto devicePtr = ci::msw::makeComUnique( device );

	::LPWSTR idStr;
	device->GetId( &idStr );
	CI_ASSERT( idStr );

	string key( ci::msw::toUtf8String( idStr ) );
	::CoTaskMemFree( idStr );
	return findDeviceByKey( key );
}

DeviceRef DeviceManagerWasapi::getDefaultInput()
{
	::IMMDeviceEnumerator *enumerator;
	HRESULT hr = ::CoCreateInstance( __uuidof(::MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(::IMMDeviceEnumerator), (void**)&enumerator );
	CI_ASSERT( hr == S_OK );
	auto enumeratorPtr = ci::msw::makeComUnique( enumerator );

	::IMMDevice *device;
	hr = enumerator->GetDefaultAudioEndpoint( eCapture, eConsole, &device );
	if( hr == E_NOTFOUND ) {
		return nullptr; // no device available
	}
	CI_ASSERT( hr == S_OK );

	auto devicePtr = ci::msw::makeComUnique( device );

	::LPWSTR idStr;
	device->GetId( &idStr );
	CI_ASSERT( idStr );

	string key( ci::msw::toUtf8String( idStr ) );
	::CoTaskMemFree( idStr );

	return findDeviceByKey( key );
}

const std::vector<DeviceRef>& DeviceManagerWasapi::getDevices()
{
	if( mDevices.empty() ) {
		parseDevices( DeviceInfo::Usage::INPUT );
		parseDevices( DeviceInfo::Usage::OUTPUT );
	}
	return mDevices;
}

std::string DeviceManagerWasapi::getName( const DeviceRef &device )
{
	return getDeviceInfo( device ).mName;
}

size_t DeviceManagerWasapi::getNumInputChannels( const DeviceRef &device )
{
	auto& devInfo = getDeviceInfo( device );
	if( devInfo.mUsage != DeviceInfo::Usage::INPUT )
		return 0;

	return devInfo.mNumChannels;
}

size_t DeviceManagerWasapi::getNumOutputChannels( const DeviceRef &device )
{
	auto& devInfo = getDeviceInfo( device );
	if( devInfo.mUsage != DeviceInfo::Usage::OUTPUT )
		return 0;	

	return devInfo.mNumChannels;
}

size_t DeviceManagerWasapi::getSampleRate( const DeviceRef &device )
{
	return getDeviceInfo( device ).mSampleRate;
}

size_t DeviceManagerWasapi::getFramesPerBlock( const DeviceRef &device )
{
	size_t frames = getDeviceInfo( device ).mFramesPerBlock;
	//return getDeviceInfo( device ).mFramesPerBlock;
	return frames;
}

// TODO: it is really only possible to change the devices samplerate in exclusive mode
// - but this is a kludge to allow context's other samplerates / block sizes until Context handles it.
void DeviceManagerWasapi::setSampleRate( const DeviceRef &device, size_t sampleRate )
{
	getDeviceInfo( device ).mSampleRate = sampleRate;

	// emitParamsWillDidChange() will be called by Device::updatFormat() next
}

void DeviceManagerWasapi::setFramesPerBlock( const DeviceRef &device, size_t framesPerBlock )
{
	// TODO: this is a bit of a kludge - we can't check if this value will actually be accepted by the IAudioClient until
	// Initialize() is called on it followed by GetBufferSize().
	// - so for now OutputDeviceNode / InputDeviceNode will later try this value, and update it as necessary
	// - later this should be done more in sync.
	getDeviceInfo( device ).mFramesPerBlock = framesPerBlock;

	// emitParamsWillDidChange() will be called by Device::updatFormat() next
}

shared_ptr<::IMMDevice> DeviceManagerWasapi::getIMMDevice( const DeviceRef &device )
{
	::IMMDeviceEnumerator *enumerator;
	HRESULT hr = ::CoCreateInstance( __uuidof(::MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(::IMMDeviceEnumerator), (void**)&enumerator );
	CI_ASSERT( hr == S_OK );
	auto enumeratorPtr = ci::msw::makeComUnique( enumerator );

	::IMMDevice *deviceImm;
	const wstring &endpointId = getDeviceInfo( device ).mEndpointId;
	hr = enumerator->GetDevice( endpointId.c_str(), &deviceImm );
	CI_ASSERT( hr == S_OK );

	return 	ci::msw::makeComShared( deviceImm );
}

void DeviceManagerWasapi::updateActualFramesPerBlock( const DeviceRef &device, size_t framesPerBlock )
{
	getDeviceInfo( device ).mFramesPerBlock = framesPerBlock;
	clearCachedValues( device );
}

// ----------------------------------------------------------------------------------------------------
// MARK: - Private
// ----------------------------------------------------------------------------------------------------

DeviceManagerWasapi::DeviceInfo& DeviceManagerWasapi::getDeviceInfo( const DeviceRef &device )
{
	return mDeviceInfoSet.at( device );
}

// This call is performed twice because a separate Device subclass is used for input and output
// and by using eRender / eCapture instead of eAll when enumerating the endpoints, it is easier
// to distinguish between the two.
// TODO: this isn't true any more, at the moment there are no Device subclasses, just DeviceManager subclasses
void DeviceManagerWasapi::parseDevices( DeviceInfo::Usage usage )
{
	const size_t kMaxPropertyStringLength = 2048;

	::IMMDeviceEnumerator *enumerator;
	const ::CLSID CLSID_MMDeviceEnumerator = __uuidof( ::MMDeviceEnumerator );
	const ::IID IID_IMMDeviceEnumerator = __uuidof( ::IMMDeviceEnumerator );
	HRESULT hr = CoCreateInstance( CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&enumerator );
	CI_ASSERT( hr == S_OK );
	auto enumeratorPtr =  ci::msw::makeComUnique( enumerator );

	::EDataFlow dataFlow = ( usage == DeviceInfo::Usage::INPUT ? eCapture : eRender );
	::IMMDeviceCollection *devices;
	hr = enumerator->EnumAudioEndpoints( dataFlow, DEVICE_STATE_ACTIVE, &devices );
	CI_ASSERT( hr == S_OK );
	auto devicesPtr = ci::msw::makeComUnique( devices );

	UINT numDevices;
	hr = devices->GetCount( &numDevices );
	CI_ASSERT( hr == S_OK );

	for ( UINT i = 0; i < numDevices; i++ )	{
		DeviceInfo devInfo;
		devInfo.mUsage = usage;

		::IMMDevice *deviceImm;
		hr = devices->Item( i, &deviceImm );
		CI_ASSERT( hr == S_OK );
		auto devicePtr = ci::msw::makeComUnique( deviceImm );

		::IPropertyStore *properties;
		hr = deviceImm->OpenPropertyStore( STGM_READ, &properties );
		CI_ASSERT( hr == S_OK );
		auto propertiesPtr = ci::msw::makeComUnique( properties );

		::PROPVARIANT nameVar;
		hr = properties->GetValue( PKEY_Device_FriendlyName, &nameVar );
		CI_ASSERT( hr == S_OK );
		devInfo.mName = ci::msw::toUtf8String( nameVar.pwszVal );

		LPWSTR endpointIdLpwStr;
		hr = deviceImm->GetId( &endpointIdLpwStr );
		CI_ASSERT( hr == S_OK );
		devInfo.mEndpointId = wstring( endpointIdLpwStr );
		devInfo.mKey = ci::msw::toUtf8String( devInfo.mEndpointId );
		::CoTaskMemFree( endpointIdLpwStr );
		
		::PROPVARIANT formatVar;
		hr = properties->GetValue( PKEY_AudioEngine_DeviceFormat, &formatVar );
		CI_ASSERT( hr == S_OK );
		::WAVEFORMATEX *format = (::WAVEFORMATEX *)formatVar.blob.pBlobData;

		devInfo.mNumChannels = format->nChannels;
		devInfo.mSampleRate = format->nSamplesPerSec;

		// activate IAudioClient to get the default device period (for frames-per-block)
		{
			::IAudioClient *audioClient;
			HRESULT hr = deviceImm->Activate( __uuidof( ::IAudioClient ), CLSCTX_ALL, NULL, (void**)&audioClient );
			ASSERT_HR_OK( hr );

			auto audioClientPtr = ci::msw::makeComUnique( audioClient );

			::REFERENCE_TIME defaultDevicePeriod; // engine time, this is for shared mode
			::REFERENCE_TIME minDevicePeriod; // this is for exclusive mode
			hr = audioClient->GetDevicePeriod( &defaultDevicePeriod, &minDevicePeriod );
			ASSERT_HR_OK( hr );

			devInfo.mFramesPerBlock = hundredNanoSecondsToFrames( defaultDevicePeriod, devInfo.mSampleRate );
		}

		DeviceRef addedDevice = addDevice( devInfo.mKey );
		auto result = mDeviceInfoSet.insert( make_pair( addedDevice, devInfo ) );
	}
}

} } } // namespace cinder::audio::msw

#endif // ( _WIN32_WINNT >= _WIN32_WINNT_VISTA )
