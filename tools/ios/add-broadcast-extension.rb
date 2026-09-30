# Adds the Broadcast Upload Extension to the Runner project `flutter create`
# generates, and gives both targets the App Group. Run by tools/scaffold-ios.sh;
# `flutter create` has no way to generate an extension target, and no Mac is
# needed to maintain this project, so the Xcode steps sender/ios/README.md used
# to list are done here instead with the xcodeproj gem CocoaPods ships.
#
#   ruby add-broadcast-extension.rb <Runner.xcodeproj> <iOS floor>
#
# Prints the app's bundle id as its last line. Idempotent: a project that
# already has the target is left alone.

require 'xcodeproj'

project_path, floor = ARGV
abort 'usage: add-broadcast-extension.rb PROJECT FLOOR' unless floor

NAME = 'KagamiBroadcast'
project = Xcodeproj::Project.open(project_path)
runner = project.targets.find { |t| t.name == 'Runner' } or abort 'no Runner target'
# From the target, not grep: RunnerTests carries a bundle id too, and which of
# the two comes first in project.pbxproj is the template's business.
host_id = runner.build_configurations.first.build_settings['PRODUCT_BUNDLE_IDENTIFIER']
abort 'Runner has no PRODUCT_BUNDLE_IDENTIFIER' if host_id.to_s.empty?
if project.targets.any? { |t| t.name == NAME }
  puts "#{NAME} already in #{project_path}"
  puts host_id
  exit 0
end
# Flutter's generated build settings, for FLUTTER_BUILD_NAME and _NUMBER: the
# extension carries the app's version. Generated.xcconfig alone, not the
# Debug/Release files that include it -- those pull in the Pods config, which
# belongs to Runner.
generated = project.files.find { |f| f.path.to_s.end_with?('Generated.xcconfig') }   or abort 'no Flutter/Generated.xcconfig in the project'

ext = project.new_target(:app_extension, NAME, :ios, floor)
group = project.main_group.new_group(NAME, NAME)
sources = Dir[File.join(File.dirname(project_path), NAME, '*.swift')].sort
abort "no Swift sources in #{NAME}/" if sources.empty?
ext.add_file_references(sources.map { |f| group.new_file(File.basename(f)) })
group.new_file('Info.plist')
group.new_file("#{NAME}.entitlements")

ext.build_configurations.each do |config|
  s = config.build_settings
  s['PRODUCT_BUNDLE_IDENTIFIER'] = "#{host_id}.broadcast"
  s['PRODUCT_NAME'] = '$(TARGET_NAME)'
  s['INFOPLIST_FILE'] = "#{NAME}/Info.plist"
  s['GENERATE_INFOPLIST_FILE'] = 'NO'
  s['CODE_SIGN_ENTITLEMENTS'] = "#{NAME}/#{NAME}.entitlements"
  s['IPHONEOS_DEPLOYMENT_TARGET'] = floor
  s['SWIFT_VERSION'] = '5.0'
  s['TARGETED_DEVICE_FAMILY'] = '1,2'
  s['APPLICATION_EXTENSION_API_ONLY'] = 'YES'
  s['SKIP_INSTALL'] = 'YES'
  # The extension's version is the app's: Apple requires the two to match,
  # and the build flags that set the app's (--build-name, --build-number) then
  # set this one too.
  config.base_configuration_reference = generated
  s['MARKETING_VERSION'] = '$(FLUTTER_BUILD_NAME)'
  s['CURRENT_PROJECT_VERSION'] = '$(FLUTTER_BUILD_NUMBER)'
  s['LD_RUNPATH_SEARCH_PATHS'] = ['$(inherited)', '@executable_path/Frameworks',
                                  '@executable_path/../../Frameworks']
end

# Embedded into the app's PlugIns folder. Placed straight after "Embed
# Frameworks" and so before Flutter's "Thin Binary" script: after it, Xcode 15
# and later report "Cycle inside Runner" and the build stops.
embed = project.new(Xcodeproj::Project::Object::PBXCopyFilesBuildPhase)
embed.name = 'Embed App Extensions'
embed.symbol_dst_subfolder_spec = :plug_ins
embed.add_file_reference(ext.product_reference, true)
  .settings = { 'ATTRIBUTES' => ['RemoveHeadersOnCopy'] }
phases = runner.build_phases
anchor = phases.index { |p| p.display_name == 'Embed Frameworks' }
thin = phases.index { |p| p.display_name == 'Thin Binary' }
at = anchor ? anchor + 1 : (thin || phases.length)
phases.insert(at, embed)
runner.add_dependency(ext)

# The app's half of the App Group, and the keep-alive (Runner/KagamiKeepAlive.m).
runner.build_configurations.each do |config|
  config.build_settings['CODE_SIGN_ENTITLEMENTS'] = 'Runner/Runner.entitlements'
end
runner_group = project.main_group.find_subpath('Runner', false) or abort 'no Runner group'
runner_group.new_file('Runner.entitlements')
runner.add_file_references([runner_group.new_file('KagamiKeepAlive.m')])

project.save
order = runner.build_phases.map(&:display_name)
puts "#{NAME} added (floor iOS #{floor}); Runner phases: #{order.join(' > ')}"
if (t = order.index('Thin Binary')) && order.index('Embed App Extensions') > t
  abort 'Embed App Extensions landed after Thin Binary'
end
puts host_id
