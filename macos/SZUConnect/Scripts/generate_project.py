#!/usr/bin/env python3
"""Generate the dependency-free, two-target Xcode project. Does not build or run."""
import pathlib, json, plistlib
root = pathlib.Path(__file__).resolve().parents[1]
objects = {}
def add(kind, **kw):
    key = f'{len(objects)+1:024X}'
    objects[key] = dict(isa=kind, **kw)
    return key
sources = ['AppModel.swift', 'EventLogFile.swift', 'NetworkIdentity.swift', 'NetworkDescriptor.swift', 'NetworkMemory.swift', 'NetworkLabels.swift', 'WiFiRecovery.swift', 'WiFiRecoveryView.swift', 'Readiness.swift', 'ConnectionPolicy.swift', 'ConnectionHealth.swift', 'DesignSystem.swift', 'ContentView.swift', 'SZUConnectApp.swift', 'DockVisibility.swift', 'SingleInstance.swift', 'PortalClient.swift', 'MenuBarPanel.swift', 'WebsiteDiagnostics.swift', 'InternetCheck.swift', 'WebsitePreferences.swift', 'RecoveryHolds.swift', 'StatusPresenter.swift', 'StatusPresenter+AppModel.swift', 'ProxyEnvironment.swift', 'Onboarding.swift', 'ConnectionPage.swift', 'RecordsPage.swift', 'SettingsPage.swift', 'Sheets.swift', 'OnboardingView.swift', 'Generated/SharedCopy.swift', 'Generated/SharedTokens.swift', 'Generated/SharedIcons.swift', 'SrunCore.cpp', 'CampusPhysicalNetwork.swift', 'CampusPhysicalTransport.swift', 'CampusPhysicalCurl.cpp']
refs = [add('PBXFileReference', lastKnownFileType='sourcecode.cpp.cpp' if s.endswith('.cpp') else 'sourcecode.swift', path=s, sourceTree='<group>') for s in sources]
source_refs = list(refs)
for header in ['SrunCore.h', 'CampusPhysicalCurl.h']:
    refs.append(add('PBXFileReference', lastKnownFileType='sourcecode.c.h',path=header,sourceTree='<group>'))
src_group = add('PBXGroup',children=refs,path='Sources',sourceTree='<group>')
assets = add('PBXFileReference',lastKnownFileType='folder.assetcatalog',path='Assets.xcassets',sourceTree='<group>')
legal_notice = add('PBXFileReference',lastKnownFileType='text',path='Resources/AutoSZUWeb-LICENSE.txt',sourceTree='<group>')
distribution_notice = add('PBXFileReference',lastKnownFileType='text',path='../../docs/DISTRIBUTION-NOTICES.txt',sourceTree='<group>')
products = add('PBXGroup',children=[],name='Products',sourceTree='<group>')
main = add('PBXGroup',children=[src_group,assets,legal_notice,distribution_notice,products],sourceTree='<group>')
base = {'CLANG_CXX_LANGUAGE_STANDARD':'c++17','CLANG_ENABLE_MODULES':'YES','SWIFT_VERSION':'5.0','SWIFT_OBJC_BRIDGING_HEADER':'Sources/SrunCore.h','SWIFT_EMIT_LOC_STRINGS':'NO','MARKETING_VERSION':'0.10.1','CURRENT_PROJECT_VERSION':'32','ENABLE_USER_SCRIPT_SANDBOXING':'YES','ASSETCATALOG_COMPILER_APPICON_NAME':'AppIcon'}
def configs(settings):
    result=[]
    for name in ['Debug','Release']:
        values = dict(settings)
        values.update({'SWIFT_OPTIMIZATION_LEVEL':'-Onone' if name=='Debug' else '-O','GCC_OPTIMIZATION_LEVEL':'0' if name=='Debug' else 's'})
        result.append(add('XCBuildConfiguration',buildSettings=values,name=name))
    return add('XCConfigurationList',buildConfigurations=result,defaultConfigurationIsVisible=0,defaultConfigurationName='Release')
targets=[]
for platform in ['macOS','iOS']:
    name='SZUConnect-'+platform
    product=add('PBXFileReference',explicitFileType='wrapper.application',includeInIndex=0,path='SZUConnect.app',sourceTree='BUILT_PRODUCTS_DIR')
    objects[products]['children'].append(product)
    phase=add('PBXSourcesBuildPhase',buildActionMask=2147483647,files=[add('PBXBuildFile',fileRef=r) for r in source_refs],runOnlyForDeploymentPostprocessing=0)
    res=add('PBXResourcesBuildPhase',buildActionMask=2147483647,files=[add('PBXBuildFile',fileRef=assets),add('PBXBuildFile',fileRef=legal_notice),add('PBXBuildFile',fileRef=distribution_notice)],runOnlyForDeploymentPostprocessing=0)
    framework=add('PBXFrameworksBuildPhase',buildActionMask=2147483647,files=[],runOnlyForDeploymentPostprocessing=0)
    setting=dict(base,PRODUCT_NAME='SZUConnect',PRODUCT_BUNDLE_IDENTIFIER='local.SZUConnect.'+('mac' if platform=='macOS' else 'ios'),INFOPLIST_FILE='Info-'+platform+'.plist',CODE_SIGN_STYLE='Automatic',GENERATE_INFOPLIST_FILE='NO')
    if platform=='macOS':
        setting.update(SDKROOT='macosx',MACOSX_DEPLOYMENT_TARGET='14.0',SUPPORTED_PLATFORMS='macosx',CODE_SIGN_IDENTITY='-',ENABLE_HARDENED_RUNTIME='YES',CODE_SIGN_ENTITLEMENTS='SZUConnect-macOS.entitlements',ARCHS='$(ARCHS_STANDARD)',OTHER_LDFLAGS='$(inherited) -lcurl')
    else:
        setting.update(SDKROOT='iphoneos',IPHONEOS_DEPLOYMENT_TARGET='17.0',SUPPORTED_PLATFORMS='iphoneos iphonesimulator',TARGETED_DEVICE_FAMILY='1,2',SUPPORTS_MACCATALYST='NO',SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD='NO')
    target=add('PBXNativeTarget',buildConfigurationList=configs(setting),buildPhases=[phase,framework,res],buildRules=[],dependencies=[],name=name,productName='SZUConnect',productReference=product,productType='com.apple.product-type.application')
    targets.append(target)
project=add('PBXProject',attributes={'BuildIndependentTargetsInParallel':'YES','LastUpgradeCheck':'2660'},buildConfigurationList=configs({}),compatibilityVersion='Xcode 14.0',developmentRegion='zh-Hans',hasScannedForEncodings=0,knownRegions=['zh-Hans','en','Base'],mainGroup=main,productRefGroup=products,projectDirPath='',projectRoot='',targets=targets)
def fmt(obj, indent=0):
    if isinstance(obj,dict): return '{\n'+''.join('\t'*(indent+1)+json.dumps(k)+ ' = '+fmt(v,indent+1)+';\n' for k,v in obj.items())+'\t'*indent+'}'
    if isinstance(obj,list): return '('+', '.join(fmt(v,indent) for v in obj)+')'
    return str(obj) if isinstance(obj,int) else json.dumps(obj,ensure_ascii=False)
folder=root/'SZUConnect.xcodeproj'; folder.mkdir(exist_ok=True)
(folder/'project.pbxproj').write_text('// !$*UTF8*$!\n'+fmt(dict(archiveVersion=1,classes={},objectVersion=56,objects=objects,rootObject=project)))
schemes=folder/'xcshareddata'/'xcschemes'; schemes.mkdir(parents=True,exist_ok=True)
for target in targets:
    name=objects[target]['name']
    ref=f'<BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{target}" BuildableName="SZUConnect.app" BlueprintName="{name}" ReferencedContainer="container:SZUConnect.xcodeproj"/>'
    (schemes/(name+'.xcscheme')).write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="2660" version="1.3"><BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries><BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">{ref}</BuildActionEntry></BuildActionEntries></BuildAction><LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" allowLocationSimulation="YES"><BuildableProductRunnable runnableDebuggingMode="0">{ref}</BuildableProductRunnable></LaunchAction><ProfileAction buildConfiguration="Release" shouldUseLaunchSchemeArgsEnv="YES" savedToolIdentifier="" useCustomWorkingDirectory="NO" debugDocumentVersioning="YES"><BuildableProductRunnable runnableDebuggingMode="0">{ref}</BuildableProductRunnable></ProfileAction><AnalyzeAction buildConfiguration="Debug"/><ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/></Scheme>''')
for platform in ['macOS','iOS']:
    info=dict(CFBundleDevelopmentRegion='zh-Hans',CFBundleDisplayName='深大随连',CFBundleExecutable='$(EXECUTABLE_NAME)',CFBundleIdentifier='$(PRODUCT_BUNDLE_IDENTIFIER)',CFBundleInfoDictionaryVersion='6.0',CFBundleName='深大随连',CFBundlePackageType='APPL',CFBundleShortVersionString='$(MARKETING_VERSION)',CFBundleVersion='$(CURRENT_PROJECT_VERSION)',NSLocalNetworkUsageDescription='检测或登录校园网时，用于访问校园网登录服务。',NSAppTransportSecurity={'NSAllowsLocalNetworking':True,'NSExceptionDomains':{'172.30.255.42':{'NSExceptionAllowsInsecureHTTPLoads':True},'www.msftconnecttest.com':{'NSExceptionAllowsInsecureHTTPLoads':True}}})
    if platform=='macOS': info.update(LSUIElement=True,LSMinimumSystemVersion='$(MACOSX_DEPLOYMENT_TARGET)',NSHighResolutionCapable=True,NSLocationUsageDescription='用于读取校园 Wi-Fi 名称，在断连时寻找已记住的网络，不采集位置坐标。',NSLocationWhenInUseUsageDescription='用于读取校园 Wi-Fi 名称，在断连时寻找已记住的网络，不采集位置坐标。',LSApplicationCategoryType='public.app-category.utilities')
    else: info.update(LSRequiresIPhoneOS=True,UIApplicationSceneManifest={'UIApplicationSupportsMultipleScenes':False,'UISceneConfigurations':{}},UILaunchScreen={},UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait','UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight'],**{'UISupportedInterfaceOrientations~ipad':['UIInterfaceOrientationPortrait','UIInterfaceOrientationPortraitUpsideDown','UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight']})
    (root/('Info-'+platform+'.plist')).write_bytes(plistlib.dumps(info))
(root/'SZUConnect-macOS.entitlements').write_bytes(plistlib.dumps({'com.apple.security.personal-information.location': True}))
print('Generated',folder)
