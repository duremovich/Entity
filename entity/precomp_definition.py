# Complete code
import uuid

class PrecompDefinition:
    def __init__(self, id: str, name: str, precomp_member: PrecompMember):
        self.id = id
        self.name = name
        self.precomp_member = precomp_member
        self.version = 1

    def update_version(self):
        self.version += 1

class PrecompDefinitionLibrary:
    def __init__(self):
        self.precomp_definitions = {}

    def add_precomp_definition(self, precomp_definition: PrecompDefinition):
        self.precomp_definitions[precomp_definition.id] = precomp_definition

    def get_precomp_definition(self, id: str):
        return self.precomp_definitions.get(id)

# Example usage
precomp_definition_library = PrecompDefinitionLibrary()
precomp_member = PrecompMember("precomp1", "Precomp 1")
precomp_definition = PrecompDefinition("precomp1", "Precomp 1", precomp_member)
precomp_definition_library.add_precomp_definition(precomp_definition)

print(precomp_definition.version)  # Output: 1
precomp_definition.update_version()
print(precomp_definition.version)  # Output: 2