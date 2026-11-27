#pragma once

// M3 Task 25 (fulla-sdk-refactor): extracted from main.cc's inline
// block of drogon::app().registerController(...) calls for every
// AutoCreation=false controller now living in libs/drogon (see PROGRESS.md
// for the AutoCreation=false mechanism-verification background). Kept as
// its own bootstrap module because the list is long (15 controllers) and
// growing it inline in main.cc would defeat the purpose of this task.

namespace bootstrap
{

// Explicitly constructs and registers every AutoCreation=false controller
// (all controllers under fulla::drogon::controllers, plus
// fulla::drogon::controllers::OAuth2StandardController) on drogon::app(). Must be
// called before drogon::app().run().
void registerAllControllers();

// M3 Task 23 (fulla-sdk-refactor, evaluation H4 "controller/filter 去
// 单例化"): looks up every already-registered controller/filter singleton
// (via drogon::app().getController<T>()/drogon::DrClassMap::
// getSingleInstance<T>()) and calls its setPlugin(OAuth2Plugin*), so
// request handlers use the cached pointer instead of calling
// drogon::app().getPlugin<OAuth2Plugin>() on every request.
//
// Ordering requirement: MUST be called from inside a
// drogon::app().registerBeginningAdvice() callback (i.e. AFTER
// drogon::app().run() has started and the OAuth2Plugin singleton has been
// constructed by config reflection -- see design.md §5.7/Task 21's
// decision and PROGRESS.md's chicken-and-egg analysis: the plugin does
// not exist before run(), while controllers/filters must be registered
// before run()). registerAllControllers() (and the filter registrations
// implied by ADD_METHOD_TO string lookups) MUST have already run.
void wireControllerPluginDependencies();

// Clears the cached OAuth2Plugin pointers on registered controllers and
// filters during server teardown so DrClassMap singletons do not retain
// dangling pointers after PluginsManager destroys OAuth2Plugin.
void clearControllerPluginDependencies();

}  // namespace bootstrap
