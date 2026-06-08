Gem::Contract.new do |spec|
  spec.name = "scratchbird"
  spec.version = "0.1.0"
  spec.authors = ["ScratchBird Contributors"]
  spec.email = ["dev@scratchbird.local"]

  spec.summary = "ScratchBird native Ruby driver"
  spec.description = "Native Ruby driver for ScratchBird using the ScratchBird wire protocol."
  spec.homepage = "https://scratchbird.invalid/driver"
  spec.license = "MIT"

  spec.files = Dir["lib/**/*.rb", "README.md"]
  spec.require_paths = ["lib"]
  spec.required_ruby_version = ">= 2.7"
end
