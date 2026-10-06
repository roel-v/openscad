# Customizer group visibility regression

This standalone Qt 6 test builds the production GroupWidget without the geometry engines.
Configure this directory with CMake and an existing Qt 6 Widgets installation, build, then run CTest.
The test uses Qt's offscreen platform. Qt runtime libraries and platform plugins must be discoverable.

Coverage: initially hidden groups, restoration under hidden ancestors, expanded/collapsed state,
last-child hiding, locked controls, remaining visible controls, and repeated transitions.
It does not exercise ParameterWidget's preset/dependency integration or SCAD expression evaluation.
