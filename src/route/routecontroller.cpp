/*****************************************************************************
* Copyright 2015-2016 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "routecontroller.h"
#include "fs/pln/flightplan.h"
#include "common/formatter.h"
#include "geo/calculations.h"
#include "gui/mainwindow.h"
#include "mapgui/mapwidget.h"
#include <QStandardItemModel>
#include <QTableView>
#include <QHeaderView>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <gui/tablezoomhandler.h>
#include <gui/widgetstate.h>
#include <mapgui/mapquery.h>
#include <QSpinBox>
#include "common/mapcolors.h"
#include "parkingdialog.h"
#include "routecommand.h"
#include "routefinder.h"
#include "routeicondelegate.h"
#include "routenetworkairway.h"
#include "routenetworkradio.h"
#include "ui_mainwindow.h"
#include <settings/settings.h>
#include <gui/actiontextsaver.h>
#include <gui/dialog.h>
#include <QListWidget>
#include <QUndoStack>
#include <QVector2D>

const int ROUTE_UNDO_LIMIT = 50;
// TODO tr
const QList<QString> ROUTE_COLUMNS({"Ident", "Region", "Name", "Airway", "Type", "Freq.\nMHz/kHz",
                                    "Course\n°M", "Direct\n°M",
                                    "Distance\nnm", "Remaining\nnm" /*, "Ground Alt\nft"*/});
namespace rc {
enum RouteColumns
{
  FIRST_COL,
  IDENT = FIRST_COL,
  REGION,
  NAME,
  AIRWAY,
  TYPE,
  FREQ,
  COURSE,
  DIRECT,
  DIST,
  REMAINING,
  // GROUND_ALT,
  LAST_COL = REMAINING
};

}

using namespace atools::fs::pln;
using namespace atools::geo;

RouteController::RouteController(MainWindow *parent, MapQuery *mapQuery, QTableView *tableView)
  : QObject(parent), parentWindow(parent), view(tableView), query(mapQuery)
{
  atools::gui::TableZoomHandler zoomHandler(view);
  Q_UNUSED(zoomHandler);

  view->setContextMenuPolicy(Qt::CustomContextMenu);

  routeNetworkRadio = new RouteNetworkRadio(query->getDatabase());
  routeNetworkAirway = new RouteNetworkAirway(query->getDatabase());

  flightplan = new Flightplan();
  undoStack = new QUndoStack(parentWindow);
  undoStack->setUndoLimit(ROUTE_UNDO_LIMIT);

  QAction *undoAction = undoStack->createUndoAction(parentWindow, "Undo Route");
  undoAction->setIcon(QIcon(":/littlenavmap/resources/icons/undo.svg"));

  QAction *redoAction = undoStack->createRedoAction(parentWindow, "Redo Route");
  redoAction->setIcon(QIcon(":/littlenavmap/resources/icons/redo.svg"));

  Ui::MainWindow *ui = parentWindow->getUi();
  ui->routeToolBar->insertAction(ui->actionRouteSelectParking, undoAction);
  ui->routeToolBar->insertAction(ui->actionRouteSelectParking, redoAction);

  connect(ui->spinBoxRouteSpeed, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, &RouteController::updateLabel);
  connect(ui->spinBoxRouteAlt, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, &RouteController::routeAltChanged);
  connect(ui->comboBoxRouteType, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
          this, &RouteController::routeTypeChanged);

  connect(view, &QTableView::doubleClicked, this, &RouteController::doubleClick);
  connect(view, &QTableView::customContextMenuRequested, this, &RouteController::tableContextMenu);

  view->horizontalHeader()->setSectionsMovable(true);
  view->verticalHeader()->setSectionsMovable(false);
  view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

  dockWindowTitle = ui->dockWidgetRoute->windowTitle();
  model = new QStandardItemModel();
  QItemSelectionModel *m = view->selectionModel();
  view->setModel(model);
  delete m;

  iconDelegate = new RouteIconDelegate(routeMapObjects);
  view->setItemDelegateForColumn(0, iconDelegate);

  // Avoid stealing of keys from other default menus
  ui->actionRouteLegDown->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteLegUp->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteDeleteLeg->setShortcutContext(Qt::WidgetWithChildrenShortcut);

  view->addActions({ui->actionRouteLegDown, ui->actionRouteLegUp, ui->actionRouteDeleteLeg});

  void (RouteController::*selChangedPtr)(const QItemSelection &selected, const QItemSelection &deselected) =
    &RouteController::tableSelectionChanged;
  connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, selChangedPtr);

  connect(ui->actionRouteLegDown, &QAction::triggered, this, &RouteController::moveLegsDown);
  connect(ui->actionRouteLegUp, &QAction::triggered, this, &RouteController::moveLegsUp);
  connect(ui->actionRouteDeleteLeg, &QAction::triggered, this, &RouteController::deleteLegs);
}

RouteController::~RouteController()
{
  delete model;
  delete iconDelegate;
  delete flightplan;
  delete undoStack;
  delete routeNetworkRadio;
  delete routeNetworkAirway;
}

void RouteController::routeAltChanged()
{
  RouteCommand *undoCommand = preChange("Change Altitude", rctype::ALTITUDE);

  updateFlightplanData();

  postChange(undoCommand);

  updateWindowTitle();
  emit routeChanged(false);
}

void RouteController::routeTypeChanged()
{
  RouteCommand *undoCommand = preChange("Change Type");

  updateFlightplanData();

  postChange(undoCommand);

  updateWindowTitle();
  emit routeChanged(false);
}

void RouteController::selectDepartureParking()
{
  const maptypes::MapAirport& airport = routeMapObjects.first().getAirport();
  ParkingDialog dialog(parentWindow, query, airport);

  int result = dialog.exec();
  dialog.hide();

  if(result == QDialog::Accepted)
  {
    maptypes::MapParking parking;
    if(dialog.getSelectedParking(parking))
      routeSetParking(parking);
  }
}

void RouteController::saveState()
{
  Ui::MainWindow *ui = parentWindow->getUi();

  atools::gui::WidgetState saver("Route/View");
  saver.save({view, ui->spinBoxRouteSpeed});

  atools::settings::Settings::instance()->setValue("Route/Filename", routeFilename);
}

void RouteController::restoreState()
{
  QString newRouteFilename = atools::settings::Settings::instance()->value("Route/Filename").toString();

  if(!newRouteFilename.isEmpty())
  {
    if(QFile::exists(newRouteFilename))
      loadFlightplan(newRouteFilename);
    else
    {
      routeFilename.clear();
      routeMapObjects.clear();
    }
  }

  Ui::MainWindow *ui = parentWindow->getUi();
  atools::gui::WidgetState saver("Route/View");
  model->setHorizontalHeaderLabels(ROUTE_COLUMNS);
  saver.restore({view, ui->spinBoxRouteSpeed});
}

void RouteController::getSelectedRouteMapObjects(QList<RouteMapObject>& selRouteMapObjects) const
{
  QItemSelection sm = view->selectionModel()->selection();
  for(const QItemSelectionRange& rng : sm)
    for(int row = rng.top(); row <= rng.bottom(); ++row)
      selRouteMapObjects.append(routeMapObjects.at(row));
}

void RouteController::newFlightplan()
{
  clearRoute();

  createRouteMapObjects();
  updateModel();
  updateWindowTitle();
  updateLabel();
  emit routeChanged(true);
}

void RouteController::loadFlightplan(const QString& filename)
{
  clearRoute();

  routeFilename = filename;
  flightplan->load(filename);
  createRouteMapObjects();
  updateModel();
  updateWindowTitle();
  updateLabel();
  emit routeChanged(true);
}

void RouteController::saveFlighplanAs(const QString& filename)
{
  routeFilename = filename;
  flightplan->save(filename);
  changed = false;
  updateWindowTitle();
}

void RouteController::saveFlightplan()
{
  flightplan->save(routeFilename);
  changed = false;
  updateWindowTitle();
}

void RouteController::calculateDirect()
{
  qDebug() << "calculateDirect";
  RouteCommand *undoCommand = preChange("Direct Route");

  flightplan->setRouteType(atools::fs::pln::DIRECT);
  flightplan->getEntries().erase(flightplan->getEntries().begin() + 1, flightplan->getEntries().end() - 1);

  createRouteMapObjects();
  updateModel();
  updateLabel();
  postChange(undoCommand);
  updateWindowTitle();
  emit routeChanged(true);
}

void RouteController::calculateRadionav()
{
  qDebug() << "calculateRadionav";
  // Changing mode might need a clear
  routeNetworkRadio->setMode(nw::ROUTE_NDB | nw::ROUTE_VOR | nw::ROUTE_VORDME);

  RouteFinder routeFinder(routeNetworkRadio);

  calculateRouteInternal(&routeFinder, atools::fs::pln::VOR, "Radionnav route", false, false);
}

void RouteController::calculateHighAlt()
{
  qDebug() << "calculateHighAlt";
  routeNetworkAirway->setMode(nw::ROUTE_JET);

  RouteFinder routeFinder(routeNetworkAirway);

  calculateRouteInternal(&routeFinder, atools::fs::pln::HIGH_ALT, "High altitude route", true, false);
}

void RouteController::calculateLowAlt()
{
  qDebug() << "calculateLowAlt";
  routeNetworkAirway->setMode(nw::ROUTE_VICTOR);

  RouteFinder routeFinder(routeNetworkAirway);

  calculateRouteInternal(&routeFinder, atools::fs::pln::LOW_ALT, "Low altitude route", true, false);
}

void RouteController::calculateSetAlt()
{
  qDebug() << "calculateSetAlt";
  routeNetworkAirway->setMode(nw::ROUTE_VICTOR | nw::ROUTE_JET);

  RouteFinder routeFinder(routeNetworkAirway);

  atools::fs::pln::RouteType type;
  if(flightplan->getCruisingAlt() > 20000)
    type = atools::fs::pln::HIGH_ALT;
  else
    type = atools::fs::pln::LOW_ALT;

  calculateRouteInternal(&routeFinder, type, "Low altitude route", true, true);
}

void RouteController::calculateRouteInternal(RouteFinder *routeFinder, atools::fs::pln::RouteType type,
                                             const QString& commandName, bool fetchAirways,
                                             bool useSetAltitude)
{
  QVector<rf::RouteEntry> route;

  QGuiApplication::setOverrideCursor(Qt::WaitCursor);

  int altitude = 0;
  if(useSetAltitude)
    altitude = flightplan->getCruisingAlt();

  routeFinder->calculateRoute(flightplan->getDeparturePos(),
                              flightplan->getDestinationPos(), route,
                              altitude);

  if(!route.isEmpty())
  {
    RouteCommand *undoCommand = preChange(commandName);

    QList<FlightplanEntry>& entries = flightplan->getEntries();

    flightplan->setRouteType(type);
    // Erase all but start and destination
    entries.erase(flightplan->getEntries().begin() + 1, entries.end() - 1);

    int minAltitude = 0;
    maptypes::MapSearchResult result;
    for(const rf::RouteEntry& routeEntry : route)
    {
      qDebug() << "Route id" << routeEntry.ref.id << "type" << routeEntry.ref.type;

      query->getMapObjectById(result, routeEntry.ref.type, routeEntry.ref.id);

      FlightplanEntry flightplanEentry;
      buildFlightplanEntry(routeEntry.ref.id, atools::geo::EMPTY_POS, routeEntry.ref.type, flightplanEentry,
                           fetchAirways);

      if(fetchAirways && routeEntry.airwayId != -1)
      {
        int alt = 0;
        updateFlightplanEntryAirway(routeEntry.airwayId, flightplanEentry, alt);
        minAltitude = std::max(minAltitude, alt);
      }

      entries.insert(entries.end() - 1, flightplanEentry);

    }
    if(minAltitude != 0)
      flightplan->setCruisingAlt(minAltitude);

    QGuiApplication::restoreOverrideCursor();
    createRouteMapObjects();
    updateModel();
    updateLabel();
    postChange(undoCommand);
    updateWindowTitle();
    emit routeChanged(true);
  }
  else
  {
    QGuiApplication::restoreOverrideCursor();
    QMessageBox::information(parentWindow,
                             QApplication::applicationName(),
                             "Routing failed. Start or destination are not reachable.");
  }
}

void RouteController::reverse()
{
  qDebug() << "reverse";

  RouteCommand *undoCommand = preChange("Reverse Route", rctype::REVERSE);

  flightplan->reverse();

  createRouteMapObjects();
  updateModel();
  updateLabel();
  postChange(undoCommand);
  updateWindowTitle();
  emit routeChanged(true);
}

QString RouteController::getDefaultFilename() const
{
  QString filename;

  if(flightplan->getFlightplanType() == atools::fs::pln::IFR)
    filename = "IFR ";
  else if(flightplan->getFlightplanType() == atools::fs::pln::VFR)
    filename = "VFR ";

  if(flightplan->getDepartureAiportName().isEmpty())
    filename += flightplan->getEntries().first().getIcaoIdent();
  else
    filename += flightplan->getDepartureAiportName() + " (" + flightplan->getDepartureIdent() + ")";

  filename += " to ";

  if(flightplan->getDestinationAiportName().isEmpty())
    filename += flightplan->getEntries().last().getIcaoIdent();
  else
    filename += flightplan->getDestinationAiportName() + " (" + flightplan->getDestinationIdent() + ")";
  filename += ".pln";
  return filename;
}

bool RouteController::isFlightplanEmpty() const
{
  return flightplan->isEmpty();
}

bool RouteController::hasValidStart() const
{
  return !flightplan->isEmpty() &&
         flightplan->getEntries().first().getWaypointType() == atools::fs::pln::entry::AIRPORT;
}

bool RouteController::hasValidDestination() const
{
  return !flightplan->isEmpty() &&
         flightplan->getEntries().last().getWaypointType() == atools::fs::pln::entry::AIRPORT;
}

bool RouteController::hasEntries() const
{
  return flightplan->getEntries().size() > 2;
}

void RouteController::preDatabaseLoad()
{
  routeNetworkRadio->clear();
  routeNetworkRadio->deInitQueries();
  routeNetworkAirway->clear();
  routeNetworkAirway->deInitQueries();
}

void RouteController::postDatabaseLoad()
{
  routeNetworkRadio->initQueries();
  routeNetworkAirway->initQueries();
  createRouteMapObjects();
  updateModel();
  updateWindowTitle();
  updateLabel();
}

void RouteController::doubleClick(const QModelIndex& index)
{
  if(index.isValid())
  {
    qDebug() << "mouseDoubleClickEvent";

    const RouteMapObject& mo = routeMapObjects.at(index.row());

    if(mo.getMapObjectType() == maptypes::AIRPORT)
    {
      if(mo.getAirport().bounding.isPoint())
        emit showPos(mo.getPosition(), 2700);
      else
        emit showRect(mo.getAirport().bounding);
    }
    else
      emit showPos(mo.getPosition(), 2700);
  }
}

void RouteController::updateMoveAndDeleteActions()
{
  Ui::MainWindow *ui = parentWindow->getUi();
  QItemSelectionModel *sm = view->selectionModel();

  ui->actionRouteLegUp->setEnabled(false);
  ui->actionRouteLegDown->setEnabled(false);
  ui->actionRouteDeleteLeg->setEnabled(false);

  if(sm->hasSelection() && model->rowCount() > 0)
  {
    if(model->rowCount() > 1)
    {
      ui->actionRouteDeleteLeg->setEnabled(true);
      ui->actionRouteLegUp->setEnabled(sm->hasSelection() &&
                                       !sm->isRowSelected(0, QModelIndex()));

      ui->actionRouteLegDown->setEnabled(sm->hasSelection() &&
                                         !sm->isRowSelected(model->rowCount() - 1, QModelIndex()));
    }
    else if(model->rowCount() == 1)
      // Only one waypoint - nothing to move
      ui->actionRouteDeleteLeg->setEnabled(true);
  }
}

void RouteController::tableContextMenu(const QPoint& pos)
{
  qDebug() << "tableContextMenu";

  Ui::MainWindow *ui = parentWindow->getUi();

  atools::gui::ActionTextSaver saver({ui->actionMapNavaidRange});
  Q_UNUSED(saver);

  QModelIndex index = view->indexAt(pos);
  if(index.isValid())
  {
    const RouteMapObject& routeMapObject = routeMapObjects.at(index.row());

    QMenu menu;

    menu.addAction(ui->actionRouteLegUp);
    menu.addAction(ui->actionRouteLegDown);
    menu.addAction(ui->actionRouteDeleteLeg);

    menu.addSeparator();
    menu.addAction(ui->actionSearchSetMark);
    menu.addSeparator();

    menu.addAction(ui->actionSearchTableCopy);

    updateMoveAndDeleteActions();

    ui->actionSearchTableCopy->setEnabled(index.isValid());

    ui->actionMapRangeRings->setEnabled(true);
    ui->actionMapHideRangeRings->setEnabled(!parentWindow->getMapWidget()->getRangeRings().isEmpty());

    ui->actionMapNavaidRange->setText(tr("Show Navaid Ranges "));
    ui->actionMapNavaidRange->setEnabled(false);
    QList<RouteMapObject> selectedRouteMapObjects;
    getSelectedRouteMapObjects(selectedRouteMapObjects);
    for(const RouteMapObject& rmo : selectedRouteMapObjects)
      if(rmo.getMapObjectType() == maptypes::VOR || rmo.getMapObjectType() == maptypes::NDB)
      {
        ui->actionMapNavaidRange->setEnabled(true);
        break;
      }

    menu.addAction(ui->actionSearchTableSelectAll);

    menu.addSeparator();
    menu.addAction(ui->actionSearchResetView);

    menu.addSeparator();
    menu.addAction(ui->actionMapRangeRings);
    menu.addAction(ui->actionMapNavaidRange);
    menu.addAction(ui->actionMapHideRangeRings);

    QAction *action = menu.exec(QCursor::pos());
    if(action != nullptr)
    {
      if(action == ui->actionSearchResetView)
      {
        // Reorder columns to match model order
        QHeaderView *header = view->horizontalHeader();
        for(int i = 0; i < header->count(); ++i)
          header->moveSection(header->visualIndex(i), i);

        view->resizeColumnsToContents();
      }
      else if(action == ui->actionSearchTableSelectAll)
        view->selectAll();
      else if(action == ui->actionSearchSetMark)
        emit changeMark(routeMapObject.getPosition());
      else if(action == ui->actionMapRangeRings)
        parentWindow->getMapWidget()->addRangeRing(routeMapObject.getPosition());
      else if(action == ui->actionMapNavaidRange)
      {

        for(const RouteMapObject& rmo : selectedRouteMapObjects)
        {
          if(rmo.getMapObjectType() == maptypes::VOR || rmo.getMapObjectType() == maptypes::NDB)
            parentWindow->getMapWidget()->addNavRangeRing(rmo.getPosition(), rmo.getMapObjectType(),
                                                          rmo.getIdent(), rmo.getFrequency(),
                                                          rmo.getRange());
        }

      }
      else if(action == ui->actionMapHideRangeRings)
        parentWindow->getMapWidget()->clearRangeRings();
    }

  }
}

void RouteController::tableSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
  Q_UNUSED(selected);
  Q_UNUSED(deselected);

  updateMoveAndDeleteActions();
  tableSelectionChanged();
}

void RouteController::tableSelectionChanged()
{
  QItemSelectionModel *sm = view->selectionModel();

  int selectedRows = 0;
  if(sm != nullptr && sm->hasSelection())
    selectedRows = sm->selectedRows().size();

  emit routeSelectionChanged(selectedRows, model->rowCount());
}

void RouteController::changeRouteUndoRedo(const atools::fs::pln::Flightplan& newFlightplan)
{
  *flightplan = newFlightplan;

  createRouteMapObjects();
  updateModel();
  updateWindowTitle();
  updateLabel();
  updateMoveAndDeleteActions();
  emit routeChanged(true);
}

void RouteController::moveLegsDown()
{
  qDebug() << "Leg down";
  moveLegsInternal(1);
}

void RouteController::moveLegsUp()
{
  qDebug() << "Leg up";
  moveLegsInternal(-1);
}

void RouteController::moveLegsInternal(int dir)
{
  QList<int> rows;
  selectedRows(rows, dir > 0);

  if(!rows.isEmpty())
  {
    RouteCommand *undoCommand = preChange("Move Waypoints", rctype::MOVE);

    QModelIndex curIdx = view->currentIndex();
    view->selectionModel()->clear();
    for(int row : rows)
    {
      flightplan->getEntries().move(row, row + dir);
      routeMapObjects.move(row, row + dir);
      model->insertRow(row + dir, model->takeRow(row));
    }
    updateRouteMapObjects();
    updateFlightplanData();
    updateModel();
    updateLabel();
    view->setCurrentIndex(model->index(curIdx.row() + dir, curIdx.column()));
    select(rows, dir);
    updateMoveAndDeleteActions();

    postChange(undoCommand);
    updateWindowTitle();

    emit routeChanged(true);
  }
}

void RouteController::routeDelete(int routeIndex, maptypes::MapObjectTypes type)
{
  qDebug() << "route delete routeIndex" << routeIndex << "type" << type;

  RouteCommand *undoCommand = preChange("Delete");

  flightplan->getEntries().removeAt(routeIndex);

  routeMapObjects.removeAt(routeIndex);

  updateRouteMapObjects();
  updateFlightplanData();
  updateModel();
  updateLabel();

  postChange(undoCommand);
  updateWindowTitle();

  emit routeChanged(true);
}

void RouteController::deleteLegs()
{
  qDebug() << "Leg delete";
  QList<int> rows;
  selectedRows(rows, true);

  if(!rows.isEmpty())
  {
    RouteCommand *undoCommand = preChange("Delete Waypoints", rctype::DELETE);

    int firstRow = rows.last();
    view->selectionModel()->clear();
    for(int row : rows)
    {
      flightplan->getEntries().removeAt(row);
      routeMapObjects.removeAt(row);
      model->removeRow(row);
    }
    updateRouteMapObjects();
    updateFlightplanData();
    updateModel();
    updateLabel();

    view->setCurrentIndex(model->index(firstRow, 0));
    updateMoveAndDeleteActions();

    postChange(undoCommand);
    updateWindowTitle();

    emit routeChanged(true);
  }
}

void RouteController::selectedRows(QList<int>& rows, bool reverse)
{
  QItemSelection sm = view->selectionModel()->selection();
  for(const QItemSelectionRange& rng : sm)
    for(int row = rng.top(); row <= rng.bottom(); row++)
      rows.append(row);

  if(!rows.isEmpty())
  {
    // Remove from bottom to top - otherwise model creates a mess
    std::sort(rows.begin(), rows.end());
    if(reverse)
      std::reverse(rows.begin(), rows.end());
  }
}

void RouteController::select(QList<int>& rows, int offset)
{
  QItemSelection newSel;

  for(int row : rows)
    newSel.append(QItemSelectionRange(model->index(row + offset, rc::FIRST_COL),
                                      model->index(row + offset, rc::LAST_COL)));

  view->selectionModel()->select(newSel, QItemSelectionModel::ClearAndSelect);
}

void RouteController::routeSetParking(maptypes::MapParking parking)
{
  qDebug() << "route set parking id" << parking.id;

  RouteCommand *undoCommand = preChange("Set Parking");

  if(routeMapObjects.isEmpty() || routeMapObjects.first().getMapObjectType() != maptypes::AIRPORT ||
     routeMapObjects.first().getId() != parking.airportId)
  {
    // No route, no start airport or different airport
    maptypes::MapAirport ap;
    query->getAirportById(ap, parking.airportId);
    routeSetStartInternal(ap);
  }

  // Update the current airport which is new or the same as the one used by the parking spot
  flightplan->setDepartureParkingName(maptypes::parkingNameForFlightplan(parking));
  routeMapObjects.first().updateParking(parking);

  updateRouteMapObjects();
  updateFlightplanData();
  updateModel();
  updateLabel();

  postChange(undoCommand);
  updateWindowTitle();

  emit routeChanged(true);
}

void RouteController::routeSetStartInternal(const maptypes::MapAirport& airport)
{
  FlightplanEntry entry;
  buildFlightplanEntry(airport, entry);

  if(!flightplan->isEmpty())
  {
    const FlightplanEntry& first = flightplan->getEntries().first();
    if(first.getWaypointType() == entry::AIRPORT &&
       flightplan->getDepartureIdent() == first.getIcaoIdent() && flightplan->getEntries().size() > 1)
    {
      flightplan->getEntries().removeAt(0);
      routeMapObjects.removeAt(0);
    }
  }

  flightplan->getEntries().prepend(entry);

  RouteMapObject rmo(flightplan, parentWindow->getElevationModel());
  rmo.loadFromAirport(&flightplan->getEntries().first(), airport, nullptr);
  routeMapObjects.prepend(rmo);
}

void RouteController::routeSetDest(maptypes::MapAirport airport)
{
  qDebug() << "route set dest id" << airport.id;

  RouteCommand *undoCommand = preChange("Set Destination");

  FlightplanEntry entry;
  buildFlightplanEntry(airport, entry);

  if(!flightplan->isEmpty())
  {
    const FlightplanEntry& last = flightplan->getEntries().last();
    if(last.getWaypointType() == entry::AIRPORT &&
       flightplan->getDestinationIdent() == last.getIcaoIdent() && flightplan->getEntries().size() > 1)
    {
      // Remove the last airport if it is set as destination
      flightplan->getEntries().removeLast();
      routeMapObjects.removeLast();
    }
  }

  flightplan->getEntries().append(entry);

  const RouteMapObject *rmoPred = nullptr;
  if(flightplan->getEntries().size() > 1)
    rmoPred = &routeMapObjects.at(routeMapObjects.size() - 1);

  RouteMapObject rmo(flightplan, parentWindow->getElevationModel());
  rmo.loadFromAirport(&flightplan->getEntries().last(), airport, rmoPred);
  routeMapObjects.append(rmo);

  updateRouteMapObjects();
  updateFlightplanData();
  updateModel();
  updateLabel();

  postChange(undoCommand);
  updateWindowTitle();

  emit routeChanged(true);
}

void RouteController::routeSetStart(maptypes::MapAirport airport)
{
  qDebug() << "route set start id" << airport.id;

  RouteCommand *undoCommand = preChange("Set Departure");

  routeSetStartInternal(airport);

  // Reset parking
  flightplan->setDepartureParkingName(QString());
  routeMapObjects.first().updateParking(maptypes::MapParking());

  updateRouteMapObjects();
  updateFlightplanData();
  updateModel();
  updateLabel();

  postChange(undoCommand);
  updateWindowTitle();

  emit routeChanged(true);
}

void RouteController::routeReplace(int id, atools::geo::Pos userPos, maptypes::MapObjectTypes type,
                                   int legIndex)
{
  qDebug() << "route replace" << "user pos" << userPos << "id" << id
           << "type" << type << "old index" << legIndex;

  RouteCommand *undoCommand = preChange("Change Waypoint");

  FlightplanEntry entry;
  buildFlightplanEntry(id, userPos, type, entry);

  flightplan->getEntries().replace(legIndex, entry);

  const RouteMapObject *rmoPred = nullptr;

  RouteMapObject rmo(flightplan, parentWindow->getElevationModel());
  rmo.loadFromDatabaseByEntry(&flightplan->getEntries()[legIndex], query, rmoPred);

  routeMapObjects.replace(legIndex, rmo);

  updateRouteMapObjects();
  updateFlightplanData();
  updateModel();
  updateLabel();

  postChange(undoCommand);
  updateWindowTitle();

  emit routeChanged(true);

}

void RouteController::routeAdd(int id, atools::geo::Pos userPos, maptypes::MapObjectTypes type, int legIndex)
{
  qDebug() << "route add id" << id << "type" << type;

  RouteCommand *undoCommand = preChange("Add Waypoint");

  FlightplanEntry entry;
  buildFlightplanEntry(id, userPos, type, entry);

  int insertIndex = -1;
  if(legIndex != -1)
    insertIndex = legIndex + 1;
  else
  {
    int leg = nearestLeg(entry.getPosition());
    qDebug() << "nearestLeg" << leg;

    insertIndex = leg;
    if(flightplan->isEmpty() || insertIndex == -1)
      insertIndex = 0;
  }
  flightplan->getEntries().insert(insertIndex, entry);

  const RouteMapObject *rmoPred = nullptr;

  if(flightplan->isEmpty() && insertIndex > 0)
    rmoPred = &routeMapObjects.at(insertIndex - 1);
  RouteMapObject rmo(flightplan, parentWindow->getElevationModel());
  rmo.loadFromDatabaseByEntry(&flightplan->getEntries()[insertIndex], query, rmoPred);

  routeMapObjects.insert(insertIndex, rmo);

  updateRouteMapObjects();
  updateFlightplanData();
  updateModel();
  updateLabel();

  postChange(undoCommand);
  updateWindowTitle();

  emit routeChanged(true);
}

int RouteController::nearestLeg(const atools::geo::Pos& pos)
{
  int nearest = -1;
  float minDistance = std::numeric_limits<float>::max();
  for(int i = 1; i < routeMapObjects.size(); i++)
  {
    bool valid;
    float distance = std::abs(pos.distanceMeterToLine(routeMapObjects.at(i - 1).getPosition(),
                                                      routeMapObjects.at(i).getPosition(), valid));

    if(valid && distance < minDistance)
    {
      minDistance = distance;
      nearest = i;
    }
  }
  for(int i = 0; i < routeMapObjects.size(); i++)
  {
    float distance = routeMapObjects.at(i).getPosition().distanceMeterTo(pos);
    if(distance < minDistance)
    {
      minDistance = distance;
      nearest = i + 1;
    }
  }
  return nearest;
}

void RouteController::buildFlightplanEntry(const maptypes::MapAirport& airport, FlightplanEntry& entry)
{
  entry.setIcaoIdent(airport.ident);
  entry.setPosition(airport.position);
  entry.setWaypointType(entry::AIRPORT);
  entry.setWaypointId(entry.getIcaoIdent());
}

void RouteController::updateFlightplanEntryAirway(int airwayId, FlightplanEntry& entry, int& minAltitude)
{
  maptypes::MapAirway airway;
  query->getAirwayById(airway, airwayId);
  entry.setAirway(airway.name);
  minAltitude = airway.minalt;
}

void RouteController::buildFlightplanEntry(int id, atools::geo::Pos userPos, maptypes::MapObjectTypes type,
                                           FlightplanEntry& entry, bool resolveWaypoints)
{
  maptypes::MapSearchResult result;
  query->getMapObjectById(result, type, id);

  if(type == maptypes::AIRPORT)
  {
    const maptypes::MapAirport& ap = result.airports.first();
    entry.setIcaoIdent(ap.ident);
    entry.setPosition(ap.position);
    entry.setWaypointType(entry::AIRPORT);
    entry.setWaypointId(entry.getIcaoIdent());
  }
  else if(type == maptypes::PARKING)
  {
    // TODO is this ever called with parking
  }
  else if(type == maptypes::WAYPOINT)
  {
    const maptypes::MapWaypoint& wp = result.waypoints.first();

    if(resolveWaypoints && wp.type == "VOR")
    {
      // Convert waypoint to underlying VOR for airway routes
      maptypes::MapVor vor;
      query->getVorForWaypoint(vor, wp.id);

      entry.setIcaoIdent(vor.ident);
      entry.setPosition(vor.position);
      entry.setIcaoRegion(vor.region);
      entry.setWaypointType(entry::VOR);
      entry.setWaypointId(entry.getIcaoIdent());
    }
    else if(resolveWaypoints && wp.type == "NDB")
    {
      // Convert waypoint to underlying NDB for airway routes
      maptypes::MapNdb ndb;
      query->getNdbForWaypoint(ndb, wp.id);

      entry.setIcaoIdent(ndb.ident);
      entry.setPosition(ndb.position);
      entry.setIcaoRegion(ndb.region);
      entry.setWaypointType(entry::NDB);
      entry.setWaypointId(entry.getIcaoIdent());
    }
    else
    {
      entry.setIcaoIdent(wp.ident);
      entry.setPosition(wp.position);
      entry.setIcaoRegion(wp.region);
      entry.setWaypointType(entry::INTERSECTION);
      entry.setWaypointId(entry.getIcaoIdent());
    }
  }
  else if(type == maptypes::VOR)
  {
    const maptypes::MapVor& vor = result.vors.first();
    entry.setIcaoIdent(vor.ident);
    entry.setPosition(vor.position);
    entry.setIcaoRegion(vor.region);
    entry.setWaypointType(entry::VOR);
    entry.setWaypointId(entry.getIcaoIdent());
  }
  else if(type == maptypes::NDB)
  {
    const maptypes::MapNdb& ndb = result.ndbs.first();
    entry.setIcaoIdent(ndb.ident);
    entry.setPosition(ndb.position);
    entry.setIcaoRegion(ndb.region);
    entry.setWaypointType(entry::NDB);
    entry.setWaypointId(entry.getIcaoIdent());
  }
  else if(type == maptypes::USER)
  {
    entry.setPosition(userPos);
    entry.setWaypointType(entry::USER);
    entry.setIcaoIdent(QString());
    entry.setWaypointId("WP" + QString::number(curUserpointNumber));
  }
  else
    qWarning() << "Unknown Map object type" << type;
}

void RouteController::updateFlightplanData()
{
  if(routeMapObjects.isEmpty())
    flightplan->clear();
  else
  {
    QString departureIcao, destinationIcao;

    const RouteMapObject& firstRmo = routeMapObjects.first();
    if(firstRmo.getMapObjectType() == maptypes::AIRPORT)
    {
      departureIcao = firstRmo.getAirport().ident;
      flightplan->setDepartureAiportName(firstRmo.getAirport().name);
      flightplan->setDepartureIdent(departureIcao);

      if(!firstRmo.getParking().name.isEmpty())
        flightplan->setDepartureParkingName(maptypes::parkingNameForFlightplan(firstRmo.getParking()));
      flightplan->setDeparturePos(firstRmo.getPosition());
    }
    else
    {
      flightplan->setDepartureAiportName(QString());
      flightplan->setDepartureIdent(QString());
      flightplan->setDepartureParkingName(QString());
      flightplan->setDeparturePos(Pos());
    }

    const RouteMapObject& lastRmo = routeMapObjects.last();
    if(lastRmo.getMapObjectType() == maptypes::AIRPORT)
    {
      destinationIcao = lastRmo.getAirport().ident;
      flightplan->setDestinationAiportName(lastRmo.getAirport().name);
      flightplan->setDestinationIdent(destinationIcao);
      flightplan->setDestinationPos(lastRmo.getPosition());
    }
    else
    {
      flightplan->setDestinationAiportName(QString());
      flightplan->setDestinationIdent(QString());
      flightplan->setDestinationPos(Pos());
    }

    Ui::MainWindow *ui = parentWindow->getUi();

    // <Descr>LFHO, EDRJ</Descr>
    flightplan->setDescription(departureIcao + ", " + destinationIcao);
    // <FPType>IFR</FPType>
    flightplan->setRouteType(atools::fs::pln::VOR);
    // <RouteType>LowAlt</RouteType>
    flightplan->setFlightplanType(
      ui->comboBoxRouteType->currentIndex() == 0 ? atools::fs::pln::IFR : atools::fs::pln::VFR);
    // <CruisingAlt>19000</CruisingAlt>
    flightplan->setCruisingAlt(ui->spinBoxRouteAlt->value());
    // <Title>LFHO to EDRJ</Title>
    flightplan->setTitle(departureIcao + " to " + destinationIcao);
  }
}

void RouteController::updateRouteMapObjects()
{
  totalDistance = 0.f;
  curUserpointNumber = 0;
  // Used to number user waypoints
  RouteMapObject *last = nullptr;
  for(int i = 0; i < routeMapObjects.size(); i++)
  {
    RouteMapObject& mapobj = routeMapObjects[i];
    mapobj.update(last);
    curUserpointNumber = std::max(curUserpointNumber, mapobj.getUserpointNumber());
    totalDistance += mapobj.getDistanceTo();

    if(i == 0)
      boundingRect = Rect(mapobj.getPosition());
    else
      boundingRect.extend(mapobj.getPosition());

    last = &mapobj;
  }
  curUserpointNumber++;
}

void RouteController::createRouteMapObjects()
{
  routeMapObjects.clear();

  RouteMapObject *last = nullptr;
  totalDistance = 0.f;
  curUserpointNumber = 0;
  // Create map objects first and calculate total distance
  for(int i = 0; i < flightplan->getEntries().size(); i++)
  {
    FlightplanEntry& entry = flightplan->getEntries()[i];

    RouteMapObject mapobj(flightplan, parentWindow->getElevationModel());
    mapobj.loadFromDatabaseByEntry(&entry, query, last);
    curUserpointNumber = std::max(curUserpointNumber, mapobj.getUserpointNumber());

    if(mapobj.getMapObjectType() == maptypes::INVALID)
      qWarning() << "Entry for ident" << entry.getIcaoIdent() <<
      "region" << entry.getIcaoRegion() << "is not valid";

    totalDistance += mapobj.getDistanceTo();
    if(i == 0)
      boundingRect = Rect(mapobj.getPosition());
    else
      boundingRect.extend(mapobj.getPosition());

    routeMapObjects.append(mapobj);
    last = &routeMapObjects.last();
  }
  curUserpointNumber++;
}

void RouteController::updateModel()
{
  model->removeRows(0, model->rowCount());

  int row = 0;
  float cumulatedDistance = 0.f;
  QList<QStandardItem *> items;
  for(const RouteMapObject& mapobj : routeMapObjects)
  {
    items.clear();
    items.append(new QStandardItem(mapobj.getIdent()));
    items.append(new QStandardItem(mapobj.getRegion()));
    items.append(new QStandardItem(mapobj.getName()));
    items.append(new QStandardItem(mapobj.getFlightplanEntry()->getAirway()));

    if(mapobj.getMapObjectType() == maptypes::VOR)
    {
      QString type = mapobj.getVor().type.at(0);

      if(mapobj.getVor().dmeOnly)
        items.append(new QStandardItem("DME (" + type + ")"));
      else if(mapobj.getVor().hasDme)
        items.append(new QStandardItem("VORDME (" + type + ")"));
      else
        items.append(new QStandardItem("VOR (" + type + ")"));
    }
    else if(mapobj.getMapObjectType() == maptypes::NDB)
    {
      QString type = mapobj.getNdb().type == "COMPASS_POINT" ? "CP" : mapobj.getNdb().type;
      items.append(new QStandardItem("NDB (" + type + ")"));
    }
    else
      items.append(nullptr);

    QStandardItem *item;
    if(mapobj.getFrequency() > 0)
    {
      if(mapobj.getMapObjectType() == maptypes::VOR)
        item = new QStandardItem(QString::number(mapobj.getFrequency() / 1000.f, 'f', 2));
      else if(mapobj.getMapObjectType() == maptypes::NDB)
        item = new QStandardItem(QString::number(mapobj.getFrequency() / 100.f, 'f', 1));
      else
        item = new QStandardItem();
      item->setTextAlignment(Qt::AlignRight);
      items.append(item);
    }
    else
      items.append(nullptr);

    if(row == 0)
    {
      items.append(nullptr);
      items.append(nullptr);
      items.append(nullptr);
    }
    else
    {
      item = new QStandardItem(QString::number(mapobj.getCourseTo(), 'f', 0));
      item->setTextAlignment(Qt::AlignRight);
      items.append(item);

      item = new QStandardItem(QString::number(mapobj.getCourseToRhumb(), 'f', 0));
      item->setTextAlignment(Qt::AlignRight);
      items.append(item);

      item = new QStandardItem(QString::number(mapobj.getDistanceTo(), 'f', 1));
      item->setTextAlignment(Qt::AlignRight);
      items.append(item);
    }

    cumulatedDistance += mapobj.getDistanceTo();

    float remaining = totalDistance - cumulatedDistance;
    if(remaining < 0.f)
      remaining = 0.f;  // Catch the -0 case due to rounding errors
    item = new QStandardItem(QString::number(remaining, 'f', 1));
    item->setTextAlignment(Qt::AlignRight);
    items.append(item);

    // item = new QStandardItem(QString::number(mapobj.getGroundAltitude(), 'f', 0));
    // item->setTextAlignment(Qt::AlignRight);
    // items.append(item);

    model->appendRow(items);
    row++;
  }

  Ui::MainWindow *ui = parentWindow->getUi();

  ui->spinBoxRouteAlt->blockSignals(true);
  ui->spinBoxRouteAlt->setValue(flightplan->getCruisingAlt());
  ui->spinBoxRouteAlt->blockSignals(false);

  ui->comboBoxRouteType->blockSignals(true);
  if(flightplan->getFlightplanType() == atools::fs::pln::IFR)
    ui->comboBoxRouteType->setCurrentIndex(0);
  else if(flightplan->getFlightplanType() == atools::fs::pln::VFR)
    ui->comboBoxRouteType->setCurrentIndex(1);
  ui->comboBoxRouteType->blockSignals(false);
}

void RouteController::updateLabel()
{
  Ui::MainWindow *ui = parentWindow->getUi();
  QString startAirport("No airport"), destAirport("No airport");
  if(!flightplan->isEmpty())
  {
    if(flightplan->getEntries().first().getWaypointType() == entry::AIRPORT)
    {
      startAirport = flightplan->getDepartureAiportName() +
                     " (" + flightplan->getDepartureIdent() + ")";
      if(!flightplan->getDepartureParkingName().isEmpty())
      {
        QString park = flightplan->getDepartureParkingName().toLower();
        park[0] = park.at(0).toUpper();
        startAirport += " " + park;
      }
    }

    if(flightplan->getEntries().last().getWaypointType() == entry::AIRPORT)
      destAirport = flightplan->getDestinationAiportName() +
                    " (" + flightplan->getDestinationIdent() + ")";

    QString routeType;
    switch(flightplan->getRouteType())
    {
      case atools::fs::pln::UNKNOWN_ROUTE:
        break;
      case atools::fs::pln::LOW_ALT:
        routeType = ", Low Altitude";
        break;
      case atools::fs::pln::HIGH_ALT:
        routeType = ", High Altitude";
        break;
      case atools::fs::pln::VOR:
        routeType = ", Radionav";
        break;
      case atools::fs::pln::DIRECT:
        routeType = ", Direct";
        break;

    }
    float travelTime = totalDistance / static_cast<float>(ui->spinBoxRouteSpeed->value());
    ui->labelRouteInfo->setText("<b>" + startAirport + "</b> to <b>" + destAirport + "</b>, " +
                                QString::number(totalDistance, 'f', 0) + " nm, " +
                                formatter::formatMinutesHoursLong(travelTime) +
                                routeType
                                );
  }
  else
    ui->labelRouteInfo->setText(tr("No Flightplan loaded"));
}

void RouteController::updateWindowTitle()
{
  Ui::MainWindow *ui = parentWindow->getUi();
  ui->dockWidgetRoute->setWindowTitle(dockWindowTitle + " - " +
                                      QFileInfo(routeFilename).fileName() +
                                      (changed ? " *" : QString()));
}

void RouteController::clearRoute()
{
  flightplan->clear();
  routeMapObjects.clear();
  routeFilename.clear();
  totalDistance = 0.f;
  undoStack->clear();
  changed = false;
}

RouteCommand *RouteController::preChange(const QString& text, rctype::RouteCmdType rcType)
{
  return new RouteCommand(this, flightplan, text, rcType);
}

void RouteController::postChange(RouteCommand *undoCommand)
{
  changed = true;
  if(undoStack != nullptr)
  {
    undoCommand->setFlightplanAfter(flightplan);
    undoStack->push(undoCommand);
  }
}

void RouteController::updateElevation()
{
  qDebug() << "updateElevation";

}