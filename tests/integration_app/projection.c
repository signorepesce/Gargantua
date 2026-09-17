#include <rest_controller.h>

$get("/person-view/{id}")
PersonView person_view(int id)
{
    PersonView view = $PersonView_from($Person_find(id));
    view.label = $format("%s profile", view.name);
    return view;
}
