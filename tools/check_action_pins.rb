#!/usr/bin/env ruby
# Parse YAML syntax without constructing Ruby objects or resolving aliases.
require 'psych'
require 'open3'

class PinPolicyError < StandardError; end

def immutable_reference?(reference)
  if reference.start_with?('./')
    parts = reference.delete_prefix('./').split('/', -1)
    return !parts.empty? && parts.all? { |part| part.match?(/\A[A-Za-z0-9_.-]+\z/) && part != '..' && part != '.' }
  end
  return true if reference.match?(%r{\Adocker://[^\s@]+@sha256:[0-9a-fA-F]{64}\z})

  reference.match?(%r{\A[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*@[0-9a-fA-F]{40}\z})
end

def inspect_node(node, path, references)
  location = "#{path}:#{node.start_line + 1}"
  raise PinPolicyError, "#{location}: YAML aliases are not permitted" if node.is_a?(Psych::Nodes::Alias)

  if node.is_a?(Psych::Nodes::Mapping)
    keys = {}
    node.children.each_slice(2) do |key, value|
      unless key.is_a?(Psych::Nodes::Scalar) && (key.tag.nil? || key.tag == 'tag:yaml.org,2002:str')
        raise PinPolicyError, "#{location}: mapping keys must be strings"
      end
      if keys[key.value] || key.value == '<<'
        raise PinPolicyError, "#{path}:#{key.start_line + 1}: duplicate or merged mapping key"
      end
      keys[key.value] = true
      next unless key.value == 'uses'

      unless value.is_a?(Psych::Nodes::Scalar) && (value.tag.nil? || value.tag == 'tag:yaml.org,2002:str') && immutable_reference?(value.value)
        raise PinPolicyError, "#{path}:#{value.start_line + 1}: uses must be a local path, full commit SHA, or Docker SHA-256 digest"
      end
      references << value.value
    end
  end
  (node.children || []).each { |child| inspect_node(child, path, references) }
end

def check_file(path, references)
  raise PinPolicyError, "#{path}: symlinked manifests are not permitted" if File.symlink?(path)

  stream = Psych.parse_stream(File.read(path))
  unless stream.children.length == 1 && stream.children.first.root.is_a?(Psych::Nodes::Mapping)
    raise PinPolicyError, "#{path}: expected one YAML mapping document"
  end
  inspect_node(stream, path, references)
end

def manifest_paths
  output, status = Open3.capture2('git', 'ls-files', '-z', '--', '*.yml', '*.yaml')
  raise PinPolicyError, 'Cannot enumerate tracked YAML files' unless status.success?

  paths = output.split("\0").select do |path|
    path.match?(%r{\A\.github/workflows/[^/]+\.ya?ml\z}) || ['action.yml', 'action.yaml'].include?(File.basename(path))
  end
  raise PinPolicyError, 'No tracked workflow manifests found' if paths.empty?

  paths.sort
end

begin
  paths = ARGV.empty? ? manifest_paths : ARGV
  references = []
  paths.each { |path| check_file(path, references) }
  puts "Action pin policy passed: #{paths.length} manifests, #{references.length} uses references"
rescue PinPolicyError, Psych::Exception, SystemCallError => error
  warn error.message
  exit 1
end
